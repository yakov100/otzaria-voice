"""Brings the clips generate_colab.ipynb wrote (clips_NNNN.zip, downloaded
from Drive) into the work folder, in the layout generate.py produces: the
22.05 kHz wav, a 16 kHz copy for qc.py, and rows in clips.jsonl.

    python unpack_colab_output.py <folder with clips_*.zip> [--work ...]
"""
import argparse
import glob
import io
import json
import os
import sys
import zipfile

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    args = ap.parse_args()
    work = args.work
    for d in ("wavs", "wav16"):
        os.makedirs(os.path.join(work, d), exist_ok=True)
    clips_path = os.path.join(work, "clips.jsonl")
    done = set()
    if os.path.exists(clips_path):
        with open(clips_path, encoding="utf-8") as f:
            done = {json.loads(l)["id"] for l in f if l.strip()}
    added = 0
    with open(clips_path, "a", encoding="utf-8", newline="\n") as out:
        for z in sorted(glob.glob(os.path.join(args.folder, "clips_*.zip"))):
            with zipfile.ZipFile(z) as zf:
                rows = [json.loads(l) for l in zf.read("clips.jsonl").decode("utf-8").splitlines() if l.strip()]
                for row in rows:
                    if row["id"] in done:
                        continue
                    data = zf.read(row["id"] + ".wav")
                    x, rate = sf.read(io.BytesIO(data), dtype="float32")
                    with open(os.path.join(work, "wavs", row["id"] + ".wav"), "wb") as f:
                        f.write(data)
                    y16 = resample_poly(x, 16000, rate) if rate != 16000 else x
                    sf.write(os.path.join(work, "wav16", row["id"] + ".wav"), np.clip(y16, -1, 1), 16000, subtype="PCM_16")
                    out.write(json.dumps(row, ensure_ascii=False) + "\n")
                    done.add(row["id"])
                    added += 1
            print(f"{os.path.basename(z)}: ok", file=sys.stderr)
    print(f"added {added} clips", file=sys.stderr)


if __name__ == "__main__":
    main()
