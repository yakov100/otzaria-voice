"""Writes an .otzvoice file (read by narrator::Voice::load) from the arrays the
Chatterbox speech encoder makes from a recording.

    python make_voice.py voice.npz out.otzvoice

voice.npz holds cond_emb (float32 [1,n,1024]), prompt_token (int64 [1,k]),
ref_x_vector (float32 [1,192]) and prompt_feat (float32 [1,frames,80]).
Format: "OTZVOICE", uint32 version 1, uint32 count, then per array: uint32
name length, name, uint32 ONNX element type (1 float32, 7 int64), uint32
rank, int64 dims, raw little-endian data.
"""
import struct
import sys

import numpy as np

NAMES = ("cond_emb", "prompt_token", "ref_x_vector", "prompt_feat")
TYPES = {np.dtype("float32"): 1, np.dtype("int64"): 7}

src, dst = sys.argv[1], sys.argv[2]
arrays = np.load(src)
with open(dst, "wb") as f:
    f.write(b"OTZVOICE" + struct.pack("<II", 1, len(NAMES)))
    for name in NAMES:
        a = np.ascontiguousarray(arrays[name])
        f.write(struct.pack("<I", len(name)) + name.encode())
        f.write(struct.pack("<II", TYPES[a.dtype], a.ndim))
        f.write(struct.pack(f"<{a.ndim}q", *a.shape))
        f.write(a.astype(a.dtype.newbyteorder("<")).tobytes())
print(dst)
