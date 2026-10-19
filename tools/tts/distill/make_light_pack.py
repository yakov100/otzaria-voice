"""Builds the downloads of the light voice (voicemodel.cpp kLight, kNakdanPack).

    python make_light_pack.py voice <dir with chaim-light.onnx/.json> <out.zip>
    python make_light_pack.py nakdan <dir with nakdan.onnx, nakdan_tokenizer.json, nikud_lexicon.bin> <out.zip>

The files go in the zip's root (the service moves the root's files into
tts/light or tts/nakdan), with LICENSES.txt beside them. Prints the size and
SHA-256 that the Release in voicemodel.cpp must name.
"""
import hashlib
import json
import sys
import zipfile
from pathlib import Path

kind, src, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
here = Path(__file__).parent
if kind == "voice":
    files = sorted(p.name for p in src.glob("*-light.onnx"))
    assert files, "no <id>-light.onnx"
    files += [f[:-5] + ".json" for f in files]
    for f in files:
        if f.endswith(".json"):
            cfg = json.loads((src / f).read_text(encoding="utf-8"))
            assert "phoneme_id_map" in cfg and "inference" in cfg, f
            print(f, "inference", cfg["inference"])
    licenses = here / "light_voice_LICENSES.txt"
elif kind == "nakdan":
    files = ["nakdan.onnx", "nakdan_tokenizer.json", "nikud_lexicon.bin"]
    licenses = here / "nakdan_LICENSES.txt"
else:
    sys.exit("kind: voice | nakdan")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in files:
        z.write(src / name, name)
    z.write(licenses, "LICENSES.txt")

h = hashlib.sha256()
with open(out, "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
print(f"size {out.stat().st_size}")
print(f"sha256 {h.hexdigest()}")
