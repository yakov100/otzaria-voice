"""Packs the passing clips for training: dataset.zip with wavs/<id>.wav and
metadata.csv (from qc.py), plus test_sentences.txt (vocalized, with marks)
for listening after training.

    python make_dataset_zip.py [--work ...] [--out dataset.zip] [--max-hours 0]
"""
import argparse
import os
import random
import zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--max-hours", type=float, default=0, help="cap the synthetic clips (0 = all); real clips always go in")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    work = args.work
    out = args.out or os.path.join(work, "dataset.zip")
    rows = []
    with open(os.path.join(work, "metadata.csv"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                rows.append(line.rstrip("\n"))
    real = [r for r in rows if r.startswith("r-")]
    synth = [r for r in rows if not r.startswith("r-")]
    if args.max_hours > 0:
        import soundfile as sf
        random.Random(args.seed).shuffle(synth)
        kept, seconds = [], 0.0
        for r in synth:
            wav = os.path.join(work, "wavs", r.split("|", 1)[0])
            seconds += sf.info(wav).duration
            kept.append(r)
            if seconds >= args.max_hours * 3600:
                break
        synth = kept
    # the listening sentences stay out of the training set, so they test generalisation
    held = set(random.Random(args.seed + 1).sample(synth, min(10, len(synth))))
    tests = [r.split("|", 1)[1] for r in held]
    rows = real + [r for r in synth if r not in held]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("metadata.csv", "\n".join(rows) + "\n")
        z.writestr("test_sentences.txt", "\n".join(tests) + "\n")
        for r in rows:
            name = r.split("|", 1)[0]
            z.write(os.path.join(work, "wavs", name), "wavs/" + name)
    print(f"{out}: {len(real)} real + {len(synth)} synthetic clips, {os.path.getsize(out) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
