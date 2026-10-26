"""Adds the speaker's own recordings (the dictation project's reading set) to
the training clips, beside the synthetic ones: the real voice, as reference
and as data.

    python import_recordings.py --rec C:\\Users\\fried\\otzaria-stt\\rec
        [--pron regular|yeshivish|all] [--work ...] [--exe ...] [--pack ...]

Reads rec/audio/manifest.jsonl (id, wav, ref) and rec/plan.jsonl (id, pron,
text_pointed), keeps one pronunciation style, adds the stress/vocal-shva
marks with `--vocalize --marks` (the text is vocalized already, so only the
marks change), and writes the clips like generate.py does, with ids r-<id>
and source "real". 16 kHz recordings are upsampled to 22.05 kHz; qc.py
measures clipping on them.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate import process_clip  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rec", required=True)
    ap.add_argument("--pron", default="regular")
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--pack", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "tts", "pack"))
    args = ap.parse_args()
    work = args.work
    exe = args.exe or os.path.join(work, "otzaria-voice.exe")

    plan = {}
    with open(os.path.join(args.rec, "plan.jsonl"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                row = json.loads(line)
                plan[row["id"]] = row
    items = []
    with open(os.path.join(args.rec, "audio", "manifest.jsonl"), encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            p = plan.get(row["id"])
            if not p or not p.get("text_pointed"):
                continue
            if args.pron != "all" and p.get("pron") != args.pron:
                continue
            wav = os.path.join(args.rec, "audio", row["wav"])
            if os.path.exists(wav):
                items.append((row["id"], wav, p["text_pointed"], p.get("pron", "")))
    print(f"{len(items)} recordings with pronunciation '{args.pron}'", file=sys.stderr)
    if not items:
        return

    # The marks, from the diacritizer, on the recordings' vocalized text.
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.txt")
        dst = os.path.join(tmp, "out.txt")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            for _, _, text, _ in items:
                f.write(text.replace("\n", " ") + "\n")
        r = subprocess.run([exe, "--vocalize", args.pack, src, dst, "--marks"])
        if r.returncode != 0:
            sys.exit("vocalize failed: " + str(r.returncode))
        with open(dst, encoding="utf-8") as f:
            marked = [l.rstrip("\n") for l in f]
    if len(marked) != len(items):
        sys.exit("vocalize returned a different number of lines")

    for d in ("wavs", "wav16"):
        os.makedirs(os.path.join(work, d), exist_ok=True)
    clips_path = os.path.join(work, "clips.jsonl")
    done = set()
    if os.path.exists(clips_path):
        with open(clips_path, encoding="utf-8") as f:
            done = {json.loads(l)["id"] for l in f if l.strip()}
    added = 0
    with open(clips_path, "a", encoding="utf-8", newline="\n") as out:
        for (rid, wav, _, pron), text in zip(items, marked):
            sid = "r-" + rid
            if sid in done:
                continue
            seconds = process_clip(wav, os.path.join(work, "wavs", sid + ".wav"),
                                   os.path.join(work, "wav16", sid + ".wav"))
            out.write(json.dumps({"id": sid, "source": "real", "book": pron, "text": text,
                                  "seconds": round(seconds, 2), "ms": 0}, ensure_ascii=False) + "\n")
            added += 1
    print(f"added {added} real clips", file=sys.stderr)


if __name__ == "__main__":
    main()
