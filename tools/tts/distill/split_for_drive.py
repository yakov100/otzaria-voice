"""Re-packs dataset.zip (make_dataset_zip.py) with FLAC audio and splits it
into parts small enough for a browser-driven upload to Google Drive (the
Claude in Chrome file_upload limit is 10MB a call; a synthetic click cannot
open the system's file picker, so the big file cannot go up in one piece):

    python split_for_drive.py [--work ...] [--out parts/] [--part-bytes 9000000]

Writes <out>/dataset-flac.zip.000, .001, ... and dataset-flac.zip.sha256.
train_colab.ipynb joins the parts in Colab, checks the hash and converts the
FLAC files back to WAV (cell 3). FLAC halves the size (334MB for the 493MB
pilot), so the upload takes half the calls.
"""
import argparse
import hashlib
import io
import os
import zipfile

import soundfile as sf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    ap.add_argument("--out", default=None, help="folder for the parts (default: <work>/parts)")
    ap.add_argument("--part-bytes", type=int, default=9_000_000)
    args = ap.parse_args()
    work = args.work
    out = args.out or os.path.join(work, "parts")
    os.makedirs(out, exist_ok=True)
    src = zipfile.ZipFile(os.path.join(work, "dataset.zip"))
    flac_zip = os.path.join(work, "dataset-flac.zip")
    with zipfile.ZipFile(flac_zip, "w", zipfile.ZIP_STORED) as z:
        for name in src.namelist():
            data = src.read(name)
            if name.endswith(".wav"):
                x, rate = sf.read(io.BytesIO(data), dtype="int16")
                buf = io.BytesIO()
                sf.write(buf, x, rate, format="FLAC", subtype="PCM_16")
                z.writestr(name[:-4] + ".flac", buf.getvalue())
            else:
                z.writestr(name, data)
    h = hashlib.sha256()
    n = 0
    with open(flac_zip, "rb") as f:
        while True:
            chunk = f.read(args.part_bytes)
            if not chunk:
                break
            h.update(chunk)
            with open(os.path.join(out, f"dataset-flac.zip.{n:03d}"), "wb") as o:
                o.write(chunk)
            n += 1
    with open(os.path.join(out, "dataset-flac.zip.sha256"), "w") as f:
        f.write(h.hexdigest() + "  dataset-flac.zip\n")
    print(f"{os.path.getsize(flac_zip) / 1e6:.0f} MB, {n} parts in {out}, sha256 {h.hexdigest()}")


if __name__ == "__main__":
    main()
