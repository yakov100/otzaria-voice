"""Generates the training audio on a rented GPU machine (generate_colab.ipynb
without Drive): Chatterbox Multilingual reads the sentences in the recorded
voice, and every 200 clips go into a zip that unpack_colab_output.py reads
on the laptop.

    source /workspace/venv-cb/bin/activate
    python generate_remote.py [--start 1500] [--end 0] [--zip-every 200]

Needs /workspace/gen-input/{sentences.tsv, sentences.vocalized.txt, ref.wav};
writes /workspace/gen-output/clips_NNNN.zip and skips ids already in them.
"""
import argparse
import glob
import io
import json
import os
import re
import time
import zipfile

import numpy as np
import soundfile as sf
import torch
import torchaudio

IN, OUT = "/workspace/gen-input", "/workspace/gen-output"
MARKS = re.compile("[ֽ֫|]")
NIKUD = re.compile("[֑-ׇ]")


def pieces(text, target=80):
    out, cur, letters, last_comma, last_space = [], "", 0, -1, -1

    def flush(upto):
        nonlocal cur, letters, last_comma, last_space
        head, cur = cur[:upto].strip(), cur[upto:]
        if head:
            out.append(head)
        letters = len(NIKUD.sub("", cur))
        last_comma = last_space = -1

    for ch in text:
        cur += " " if ch in "\n\r\t" else ch
        if not NIKUD.match(ch):
            letters += 1
        if ch in ".?!:;׃\n":
            flush(len(cur))
            continue
        if ch == ",":
            last_comma = len(cur)
        if ch == " ":
            last_space = len(cur)
        if letters >= target:
            if last_comma > len(cur) / 3:
                flush(last_comma)
            elif last_space > 0:
                flush(last_space)
            else:
                flush(len(cur))
    flush(len(cur))
    return out


def trim(x, rate, db=-40.0, pad=0.06):
    frame = int(rate * 0.01)
    n = len(x) // frame
    if n < 3:
        return x
    rms = np.sqrt((x[:n * frame].reshape(n, frame) ** 2).mean(axis=1) + 1e-12)
    loud = np.where(20 * np.log10(rms) > db)[0]
    if len(loud) == 0:
        return x
    a = max(0, loud[0] * frame - int(rate * pad))
    b = min(len(x), (loud[-1] + 1) * frame + int(rate * pad))
    return x[a:b]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--end", type=int, default=0)
    ap.add_argument("--zip-every", type=int, default=200)
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    from chatterbox.mtl_tts import ChatterboxMultilingualTTS
    model = ChatterboxMultilingualTTS.from_pretrained(device="cuda")
    model.prepare_conditionals(f"{IN}/ref.wav", exaggeration=0.5)
    rows = []
    with open(f"{IN}/sentences.tsv", encoding="utf-8") as tsv, open(f"{IN}/sentences.vocalized.txt", encoding="utf-8") as voc:
        next(tsv)
        for meta, text in zip(tsv, voc):
            sid, source, book, _, _ = meta.rstrip("\n").split("\t", 4)
            rows.append((sid, source, book, text.rstrip("\n")))
    rows = rows[args.start:args.end or None]
    done = set()
    for z in glob.glob(f"{OUT}/clips_*.zip"):
        with zipfile.ZipFile(z) as zf:
            done.update(n[:-4] for n in zf.namelist() if n.endswith(".wav"))
    todo = [r for r in rows if r[0] not in done]
    print(f"{len(rows)} lines, {len(done)} done, {len(todo)} to do", flush=True)
    resample = torchaudio.transforms.Resample(model.sr, 22050).to("cuda")
    batch, meta, zip_index = {}, [], len(glob.glob(f"{OUT}/clips_*.zip"))
    t0, seconds_total = time.time(), 0.0

    def flush_zip():
        nonlocal batch, meta, zip_index
        if not batch:
            return
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
            for sid, wav in batch.items():
                zf.writestr(sid + ".wav", wav)
            zf.writestr("clips.jsonl", "\n".join(json.dumps(m, ensure_ascii=False) for m in meta) + "\n")
        zip_index += 1
        with open(f"{OUT}/clips_{zip_index:04d}.zip", "wb") as f:
            f.write(buf.getvalue())
        print(f"  wrote clips_{zip_index:04d}.zip ({len(batch)} clips); {seconds_total / 3600:.2f} h of audio "
              f"in {(time.time() - t0) / 3600:.2f} h", flush=True)
        batch, meta = {}, []

    with torch.inference_mode():
        for i, (sid, source, book, text) in enumerate(todo, 1):
            parts = []
            for piece in pieces(MARKS.sub("", text)):
                wav = model.generate(piece, language_id="he", exaggeration=0.5, cfg_weight=0.5)
                parts.append(resample(wav.to("cuda")).squeeze(0).cpu().numpy())
                parts.append(np.zeros(22050 // 6, dtype=np.float32))
            x = trim(np.concatenate(parts[:-1]), 22050)
            peak = float(np.abs(x).max()) if len(x) else 0.0
            if peak > 0:
                x = x * (0.89 / peak)
            buf = io.BytesIO()
            sf.write(buf, np.clip(x, -1, 1), 22050, format="WAV", subtype="PCM_16")
            batch[sid] = buf.getvalue()
            seconds_total += len(x) / 22050
            meta.append({"id": sid, "source": source, "book": book, "text": text,
                         "seconds": round(len(x) / 22050, 2), "ms": 0})
            if len(batch) >= args.zip_every:
                flush_zip()
            if i % 50 == 0:
                print(f"{i}/{len(todo)}  {seconds_total / 60:.1f} min of audio, {(time.time() - t0) / 60:.1f} min elapsed", flush=True)
    flush_zip()
    print("done", flush=True)


if __name__ == "__main__":
    main()
