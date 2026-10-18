"""On the training machine: merges the generators' QC output (finish_gen.sh,
copied with `vastai copy` into <in>/<machine>/{metadata.csv, wavs/}) into one
Piper training set at <out> (metadata.csv + wavs/ as hard links), leaving out
the ten listening sentences (test_sentences.txt in <out>) and any real
recording (r-*), then prints the totals. Training follows with
train_light.py --data <out>.

    python build_full.py [--in /workspace/gen/in] [--out /workspace/gen]
"""
import argparse
import glob
import json
import os
import re
import unicodedata
import wave

MARKS = re.compile("[\u0591-\u05c7|]")


def key(text):
    return MARKS.sub("", unicodedata.normalize("NFD", text)).replace(" ", "").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="/workspace/gen/in")
    ap.add_argument("--out", default="/workspace/gen")
    args = ap.parse_args()
    tests = set()
    test_path = os.path.join(args.out, "test_sentences.txt")
    if os.path.exists(test_path):
        tests = {key(l) for l in open(test_path, encoding="utf-8") if l.strip()}
    os.makedirs(os.path.join(args.out, "wavs"), exist_ok=True)
    rows, seen, skipped, seconds, per_machine = [], set(), {"test": 0, "real": 0, "dup": 0, "missing": 0}, 0.0, {}
    for meta in sorted(glob.glob(os.path.join(args.inp, "*", "metadata.csv"))):
        machine = os.path.basename(os.path.dirname(meta))
        n = 0
        for line in open(meta, encoding="utf-8"):
            line = line.rstrip("\n")
            if not line:
                continue
            name, text = line.split("|", 1)
            src = os.path.join(os.path.dirname(meta), "wavs", name)
            if name in seen:
                skipped["dup"] += 1
                continue
            if name.startswith("r-"):
                skipped["real"] += 1
                continue
            if key(text) in tests:
                skipped["test"] += 1
                continue
            if not os.path.exists(src):
                skipped["missing"] += 1
                continue
            dst = os.path.join(args.out, "wavs", name)
            if not os.path.exists(dst):
                try:
                    os.link(src, dst)
                except OSError:
                    import shutil
                    shutil.copyfile(src, dst)
            with wave.open(dst) as w:
                seconds += w.getnframes() / w.getframerate()
            # Piper reads metadata.csv with csv.reader: a '"' opens a quoted field and swallows
            # the following lines (13,123 lines became 11,578 rows). Quote marks are not spoken.
            text = text.replace('"', "").replace("“", "").replace("”", "").replace("״", "")
            rows.append(f"{name}|{unicodedata.normalize('NFD', text)}")
            seen.add(name)
            n += 1
        per_machine[machine] = n
    with open(os.path.join(args.out, "metadata.csv"), "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    summary = {"clips": len(rows), "hours": round(seconds / 3600, 2), "per_machine": per_machine, "skipped": skipped}
    json.dump(summary, open(os.path.join(args.out, "build.json"), "w"), indent=1)
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
