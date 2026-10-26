"""Trains the tiny diacritizer on this computer's processor, or on a GPU with
--device cuda (the Colab notebook, train_colab.ipynb, does the same): a character BiLSTM with the
three heads of nakdan.onnx, from the parts make_dataset.py wrote.

    python train_local.py [--work ...] [--run v1] [--epochs 12] [--batch 64] [--threads 12]
                          [--limit N (batches per epoch, smoke test)] [--export-only]

Checkpoints go to <work>/runs/<run>/epoch-NN.pt (resumes from the newest),
the log to <work>/runs/<run>/train.log, and at the end nakdan-fp32.onnx and
the int8 nakdan.onnx that the service loads (put it in tts/nakdan beside
nakdan_tokenizer.json, or point OTZARIA_NAKDAN_DIR at a folder holding both).
Needs: pip install torch onnx onnxruntime numpy.
"""
import argparse
import glob
import hashlib
import json
import os
import random
import sys
import time

import numpy as np
import torch
import torch.nn as nn


def log(path, msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(line + "\n")


class TinyNakdan(nn.Module):
    def __init__(self, vocab, pad, emb=192, hidden=256, layers=3, dropout=0.2):
        super().__init__()
        self.emb = nn.Embedding(vocab, emb, padding_idx=pad)
        self.drop = nn.Dropout(dropout)
        self.lstm = nn.LSTM(emb, hidden, num_layers=layers, bidirectional=True, batch_first=True, dropout=dropout)
        self.nikud = nn.Linear(2 * hidden, 29)
        self.shin = nn.Linear(2 * hidden, 2)
        self.additional = nn.Linear(2 * hidden, 3)

    def forward(self, input_ids, attention_mask=None, token_type_ids=None):
        x = self.emb(input_ids)
        # The service feeds the big model's three inputs; use the two extra
        # ones (harmlessly) so that the ONNX export keeps them.
        if attention_mask is not None:
            x = x * attention_mask.unsqueeze(-1).to(x.dtype)
        if token_type_ids is not None:
            x = x + (token_type_ids.unsqueeze(-1) * 0).to(x.dtype)
        h, _ = self.lstm(self.drop(x))
        h = self.drop(h)
        return self.nikud(h), self.shin(h), self.additional(h)


def load_examples(dataset_dir):
    examples = []
    for p in sorted(glob.glob(os.path.join(dataset_dir, "part-*.npz"))):
        z = np.load(p)
        # Every z[name] re-reads the array from the zip: take each one once.
        off, src = z["offsets"], z["src"].tobytes().decode("ascii")
        ids, nikud, shin = z["ids"].astype(np.int64), z["nikud"].astype(np.int64), z["shin"].astype(np.int64)
        stress, shva = z["stress"].astype(np.float32), z["shva"].astype(np.float32)
        for i in range(len(off) - 1):
            a, b = off[i], off[i + 1]
            examples.append((src[i], ids[a:b], nikud[a:b], shin[a:b], stress[a:b], shva[a:b]))
    return examples


def is_dev(e):
    return int(hashlib.md5(e[1].tobytes()).hexdigest()[:4], 16) % 50 == 0


def collate(batch, pad):
    n = max(len(e[1]) for e in batch)

    def padded(key, fill, dtype):
        out = torch.full((len(batch), n), fill, dtype=dtype)
        for i, e in enumerate(batch):
            out[i, :len(e[key])] = torch.as_tensor(e[key])
        return out

    return {"src": [e[0] for e in batch], "ids": padded(1, pad, torch.long), "nikud": padded(2, -1, torch.long),
            "shin": padded(3, -1, torch.long), "stress": padded(4, -1, torch.float), "shva": padded(5, -1, torch.float)}


def buckets(data, batch_size):
    order = sorted(range(len(data)), key=lambda i: len(data[i][1]))
    return [[data[j] for j in order[i:i + batch_size]] for i in range(0, len(order), batch_size)]


def evaluate(model, dev_batches, pad, device="cpu"):
    model.eval()
    right = {"nikud": 0, "shin": 0, "stress": 0, "shva": 0}
    total = dict(right)
    by_src = {}
    with torch.no_grad():
        for batch in dev_batches:
            b = collate(batch, pad)
            n, s, a = model(b["ids"].to(device))
            n, s, a = n.cpu(), s.cpu(), a.cpu()
            pn, ps = n.argmax(-1), s.argmax(-1)
            pst, psh = (a[..., 0] > 0).float(), (a[..., 1] > 0).float()
            m = b["nikud"] >= 0
            right["nikud"] += (pn[m] == b["nikud"][m]).sum().item()
            total["nikud"] += m.sum().item()
            ms = b["shin"] >= 0
            right["shin"] += (ps[ms] == b["shin"][ms]).sum().item()
            total["shin"] += ms.sum().item()
            right["stress"] += (pst[m] == b["stress"][m]).sum().item()
            total["stress"] += m.sum().item()
            right["shva"] += (psh[m] == b["shva"][m]).sum().item()
            total["shva"] += m.sum().item()
            for row, src in enumerate(b["src"]):
                mm = m[row]
                r, t = by_src.get(src, (0, 0))
                by_src[src] = (r + (pn[row][mm] == b["nikud"][row][mm]).sum().item(), t + mm.sum().item())
    model.train()
    acc = {k: round(100.0 * right[k] / max(1, total[k]), 2) for k in right}
    acc["by_src"] = {k: round(100.0 * r / max(1, t), 2) for k, (r, t) in by_src.items()}
    return acc


def export(model, meta, out_dir, logp, dev, pad):
    import onnxruntime as ort
    from onnxruntime.quantization import QuantType, quantize_dynamic
    model.eval()
    n = 40
    dummy = (torch.randint(4, meta["vocab_size"], (1, n)), torch.ones(1, n, dtype=torch.long),
             torch.zeros(1, n, dtype=torch.long))
    fp32 = os.path.join(out_dir, "nakdan-fp32.onnx")
    int8 = os.path.join(out_dir, "nakdan.onnx")
    names = ["input_ids", "attention_mask", "token_type_ids", "nikud_logits", "shin_logits", "additional_logits"]
    torch.onnx.export(model, dummy, fp32, input_names=names[:3], output_names=names[3:],
                      dynamic_axes={k: {0: "batch", 1: "seq"} for k in names}, opset_version=17, dynamo=False)
    quantize_dynamic(fp32, int8, weight_type=QuantType.QInt8)
    log(logp, f"onnx: fp32 {os.path.getsize(fp32) / 1e6:.1f} MB, int8 {os.path.getsize(int8) / 1e6:.1f} MB")
    sess = ort.InferenceSession(int8)
    right = total = 0
    for e in dev[:400]:
        ids = e[1][None].astype(np.int64)
        out = sess.run(None, {"input_ids": ids, "attention_mask": np.ones_like(ids), "token_type_ids": np.zeros_like(ids)})
        pred = out[0][0].argmax(-1)
        m = e[2] >= 0
        right += int((pred[m] == e[2][m]).sum())
        total += int(m.sum())
    log(logp, f"int8 onnx on {total} dev letters: {100.0 * right / max(1, total):.2f}%; outputs "
              f"{[o.name for o in sess.get_outputs()]}")
    model.train()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "nakdan-tiny"))
    ap.add_argument("--run", default="v1")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--threads", type=int, default=12)
    ap.add_argument("--limit", type=int, default=0, help="batches per epoch (smoke test)")
    ap.add_argument("--export-only", action="store_true")
    ap.add_argument("--device", default="cpu", help="cpu, cuda, or auto")
    args = ap.parse_args()
    device = ("cuda" if torch.cuda.is_available() else "cpu") if args.device == "auto" else args.device
    torch.set_num_threads(args.threads)
    torch.manual_seed(7)
    random.seed(7)
    out_dir = os.path.join(args.work, "runs", args.run)
    os.makedirs(out_dir, exist_ok=True)
    logp = os.path.join(out_dir, "train.log")
    meta = json.load(open(os.path.join(args.work, "dataset", "meta.json")))
    pad = meta["pad"]
    examples = load_examples(os.path.join(args.work, "dataset"))
    random.Random(7).shuffle(examples)
    dev = [e for e in examples if is_dev(e)]
    train = [e for e in examples if not is_dev(e)]
    train_b, dev_b = buckets(train, args.batch), buckets(dev, args.batch)
    if args.limit:
        train_b, dev_b = train_b[:args.limit], dev_b[:max(1, args.limit // 10)]
    log(logp, f"{len(train)} train / {len(dev)} dev examples, {len(train_b)} batches an epoch, {args.threads} threads, {device}")

    model = TinyNakdan(meta["vocab_size"], pad).to(device)
    log(logp, f"{sum(p.numel() for p in model.parameters()) / 1e6:.2f}M parameters")
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=max(1, args.epochs * len(train_b)), pct_start=0.05)
    ce = nn.CrossEntropyLoss(ignore_index=-1)
    bce = nn.BCEWithLogitsLoss(reduction="none")
    start_epoch = 0
    ckpts = sorted(glob.glob(os.path.join(out_dir, "epoch-*.pt")))
    if ckpts:
        state = torch.load(ckpts[-1], map_location=device)
        model.load_state_dict(state["model"])
        if not args.export_only:
            opt.load_state_dict(state["opt"])
            sched.load_state_dict(state["sched"])
        start_epoch = state["epoch"] + 1
        log(logp, f"resumed from {ckpts[-1]}")
    if args.export_only:
        export(model.cpu(), meta, out_dir, logp, dev, pad)
        return

    model.train()
    for epoch in range(start_epoch, args.epochs):
        order = list(range(len(train_b)))
        random.shuffle(order)
        t0, loss_sum, seen = time.time(), 0.0, 0
        for k, i in enumerate(order):
            b = {k: (v.to(device) if hasattr(v, "to") else v) for k, v in collate(train_b[i], pad).items()}
            n, s, a = model(b["ids"])
            m = (b["nikud"] >= 0).float()
            loss = ce(n.reshape(-1, 29), b["nikud"].reshape(-1)) + 0.5 * ce(s.reshape(-1, 2), b["shin"].reshape(-1))
            loss = loss + (bce(a[..., 0], b["stress"].clamp(min=0)) * m).sum() / m.sum().clamp(min=1)
            loss = loss + (bce(a[..., 1], b["shva"].clamp(min=0)) * m).sum() / m.sum().clamp(min=1)
            opt.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            loss_sum += loss.item()
            seen += 1
            if seen % 100 == 0:
                rate = (time.time() - t0) / seen
                log(logp, f"epoch {epoch} step {seen}/{len(order)} loss {loss_sum / 100:.3f} "
                          f"({rate:.2f} s/step, {rate * (len(order) - seen) / 60:.0f} min left)")
                loss_sum = 0.0
        acc = evaluate(model, dev_b, pad, device)
        log(logp, f"epoch {epoch} done in {(time.time() - t0) / 60:.1f} min; dev letters {acc['nikud']}% "
                  f"shin {acc['shin']}% stress {acc['stress']}% shva {acc['shva']}% by source {acc['by_src']}")
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "sched": sched.state_dict(),
                    "epoch": epoch, "acc": acc}, os.path.join(out_dir, f"epoch-{epoch:02d}.pt"))
        for old in sorted(glob.glob(os.path.join(out_dir, "epoch-*.pt")))[:-2]:
            os.remove(old)
    export(model.cpu(), meta, out_dir, logp, dev, pad)
    log(logp, "done")


if __name__ == "__main__":
    main()
