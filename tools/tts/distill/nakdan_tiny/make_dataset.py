"""Turns the teacher's output (prepare.py: <work>/out/NNN.txt, vocalized lines
with pronunciation marks) into the tiny diacritizer's training arrays, with
the same character tokens as the big model (nakdan_tokenizer.json) and the
same label spaces as its output heads (nikud_lexicon.cpp kClasses; shin/sin;
stress, vocal shva):

    python make_dataset.py [--work ...] [--pack ...] [--max-len 512] [--part-bytes 9000000]

Writes <work>/dataset/part-NN.npz (each under --part-bytes, so a browser
upload takes them one call each) and meta.json. Every example is one line
(cut at --max-len tokens at a space): ids (uint16, with [CLS] and [SEP]),
nikud (int8: class 0..28, -1 where there is no letter), shin (int8: 0 shin,
1 sin, -1 elsewhere), stress and shva (int8: 0/1 on letters, -1 elsewhere),
src (one byte a line: b Talmud, v vocalized library, p plain library).
"""
import argparse
import io
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from build_nikud_lexicon import VOWELS, DAGESH, SIN, SHIN  # noqa: E402

STRESS, SHVA = "֫", "ֽ"
MARKS = set("֑֖֛֚֒֓֔֕֗֘֙֜֝֞֟"
            "֢֣֤֥֦֧֪֭֮֠֡֨֩֫֬֯"
            "ְֱֲֳִֵֶַָֹֺֻּֽֿ"
            "ׇׁׂׅׄ")


def is_letter(c):
    return "א" <= c <= "ת"


def class_of(marks):
    """The nikud class of a letter's marks, or None when they are outside the classes."""
    vowel = "".join(m for m in marks if m in VOWELS and m)
    if len(vowel) > 1:
        return None
    cls = VOWELS[vowel]
    if DAGESH in marks:
        if cls == 0:
            cls = 2
        elif 3 <= cls <= 14:
            cls += 12
        elif cls == 27:
            cls = 28
    return cls


def encode(line, vocab, unk):
    """Plain characters (what the model reads) and the labels per character."""
    chars, nikud, shin, stress, shva = [], [], [], [], []
    i = 0
    while i < len(line):
        c = line[i]
        i += 1
        if c in MARKS or c == "|":
            continue  # a stray mark without a letter
        marks = ""
        while i < len(line) and line[i] in MARKS:
            marks += line[i]
            i += 1
        chars.append(c)
        if is_letter(c):
            cls = class_of(marks)
            nikud.append(-1 if cls is None else cls)
            shin.append((1 if SIN in marks else 0) if c == "ש" and (SIN in marks or SHIN in marks) else -1)
            stress.append(1 if STRESS in marks else 0)
            shva.append(1 if SHVA in marks else 0)
        else:
            nikud.append(-1)
            shin.append(-1)
            stress.append(-1)
            shva.append(-1)
    ids = [vocab.get(c.lower() if "A" <= c <= "Z" else c, unk) for c in chars]
    return chars, ids, nikud, shin, stress, shva


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "nakdan-tiny"))
    ap.add_argument("--pack", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "tts", "pack"))
    ap.add_argument("--max-len", type=int, default=512)
    ap.add_argument("--part-bytes", type=int, default=9_000_000)
    ap.add_argument("--out", default=None, help="dataset folder (default <work>/dataset)")
    ap.add_argument("--exclude", default=None,
                    help="a file of lines to keep out of the training set (the evaluation holdout), matched without marks")
    args = ap.parse_args()
    excluded = set()
    if args.exclude:
        for line in open(args.exclude, encoding="utf-8"):
            key = "".join(c for c in line if c not in MARKS and c != "|").strip()
            if key:
                excluded.add(key)
    tok = json.load(open(os.path.join(args.pack, "nakdan_tokenizer.json"), encoding="utf-8"))["model"]["vocab"]
    cls_id, sep_id, unk_id = tok["[CLS]"], tok["[SEP]"], tok["[UNK]"]
    out_dir = args.out or os.path.join(args.work, "dataset")
    os.makedirs(out_dir, exist_ok=True)
    shards_dir, teach_dir = os.path.join(args.work, "shards"), os.path.join(args.work, "out")

    examples = []  # (src, ids, nikud, shin, stress, shva)
    counts = {"lines": 0, "letters": 0, "unclassed": 0, "by_src": {}, "excluded": 0}
    for name in sorted(f for f in os.listdir(teach_dir) if f.endswith(".txt")):
        lines = open(os.path.join(teach_dir, name), encoding="utf-8").read().split("\n")
        srcs = open(os.path.join(shards_dir, name[:-4] + ".src"), encoding="utf-8").read().split("\n")
        for line, src in zip(lines, srcs):
            if not line.strip():
                continue
            if excluded and "".join(c for c in line if c not in MARKS and c != "|").strip() in excluded:
                counts["excluded"] += 1
                continue
            chars, ids, nikud, shin, stress, shva = encode(line, tok, unk_id)
            # Cut at a space to --max-len - 2 tokens (room for [CLS] and [SEP]).
            start = 0
            while start < len(ids):
                end = min(len(ids), start + args.max_len - 2)
                if end < len(ids):
                    sp = max((k for k in range(start + (end - start) // 2, end) if chars[k] == " "), default=end)
                    end = sp
                sl = slice(start, end)
                examples.append((src, [cls_id] + ids[sl] + [sep_id], [-1] + nikud[sl] + [-1], [-1] + shin[sl] + [-1],
                                 [-1] + stress[sl] + [-1], [-1] + shva[sl] + [-1]))
                counts["lines"] += 1
                counts["letters"] += sum(1 for x in nikud[sl] if x >= 0)
                counts["unclassed"] += sum(1 for x, c in zip(nikud[sl], chars[sl]) if x < 0 and is_letter(c))
                counts["by_src"][src] = counts["by_src"].get(src, 0) + (end - start)
                start = end
    print(f"{counts['lines']} examples, {counts['letters']} labelled letters, {counts['unclassed']} letters "
          f"outside the classes, {counts['excluded']} holdout lines excluded, chars by source {counts['by_src']}", file=sys.stderr)

    # Parts: concatenated arrays with offsets, each part under --part-bytes.
    part, part_bytes, n = [], 0, 0
    def flush():
        nonlocal part, part_bytes, n
        if not part:
            return
        offsets = np.zeros(len(part) + 1, dtype=np.int64)
        for i, e in enumerate(part):
            offsets[i + 1] = offsets[i] + len(e[1])
        buf = io.BytesIO()
        np.savez(buf,
                 offsets=offsets,
                 src=np.frombuffer("".join(e[0] for e in part).encode("ascii"), dtype=np.uint8),
                 ids=np.concatenate([np.asarray(e[1], dtype=np.uint16) for e in part]),
                 nikud=np.concatenate([np.asarray(e[2], dtype=np.int8) for e in part]),
                 shin=np.concatenate([np.asarray(e[3], dtype=np.int8) for e in part]),
                 stress=np.concatenate([np.asarray(e[4], dtype=np.int8) for e in part]),
                 shva=np.concatenate([np.asarray(e[5], dtype=np.int8) for e in part]))
        with open(os.path.join(out_dir, f"part-{n:02d}.npz"), "wb") as f:
            f.write(buf.getvalue())
        n += 1
        part, part_bytes = [], 0
    for e in examples:
        part.append(e)
        part_bytes += len(e[1]) * 6 + 16
        if part_bytes >= args.part_bytes * 0.95:
            flush()
    flush()
    json.dump({"vocab_size": max(tok.values()) + 1, "cls": cls_id, "sep": sep_id, "unk": unk_id,
               "pad": tok.get("[PAD]", 0), "parts": n, "max_len": args.max_len, "counts": counts},
              open(os.path.join(out_dir, "meta.json"), "w"), indent=1)
    print(f"{n} parts in {out_dir}", file=sys.stderr)


if __name__ == "__main__":
    main()
