"""Builds the reading-aloud model pack (voicemodel.cpp) from a model folder.

    python make_voice_model.py <model dir> <out.zip>

<model dir> is a working narrator folder (narrator.h): the ONNX files and
their external data, the two tokenizers, and voices/*.otzvoice.
tools/voice_model_LICENSES.txt is added as LICENSES.txt. Prints the size and
SHA-256 that kRelease in voicemodel.cpp must name.
"""
import hashlib
import sys
import zipfile
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
FILES = [
    "nakdan.onnx", "nakdan_tokenizer.json", "text_tokenizer.json",
    "embed_tokens.onnx", "embed_tokens.onnx_data",
    "language_model.onnx", "language_model_q4.onnx_data",
    "decoder.onnx", "conditional_decoder_quantized.onnx_data",
]
licenses = Path(__file__).with_name("voice_model_LICENSES.txt")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
    for name in FILES:
        z.write(src / name, name)
    for voice in sorted((src / "voices").glob("*.otzvoice")):
        z.write(voice, f"voices/{voice.name}")
    z.write(licenses, "LICENSES.txt")

h = hashlib.sha256()
with open(out, "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
print(f"size {out.stat().st_size}")
print(f"sha256 {h.hexdigest()}")
