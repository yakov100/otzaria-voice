"""Makes an .otzvoice file (a recorded voice for the narrator) from a recording.

    python make_voice_from_wav.py recording.wav out.otzvoice [--speech-encoder path]

The recording: one speaker, 10-30 seconds of clean speech, no music. Any
format soundfile reads (WAV, FLAC, OGG); any sample rate (resampled here to
24 kHz); stereo is mixed down. The voice is taken from the first 10 seconds
of speech (the model's own limit), after leading silence is trimmed.

The four arrays the narrator needs (see make_voice.py and Voice::load in
service/src/tts/narrator.cpp) come straight out of the Chatterbox
Multilingual speech encoder (speech_encoder.onnx of
https://huggingface.co/onnx-community/chatterbox-multilingual-ONNX, 590 MB):
    audio_features     -> cond_emb      float32 [1, 33, 1024]
    audio_tokens       -> prompt_token  int64   [1, 25 a second, at most 250]
    speaker_embeddings -> ref_x_vector  float32 [1, 192]
    speaker_features   -> prompt_feat   float32 [1, 50 a second, 80]
The encoder is downloaded once into a cache folder (%LOCALAPPDATA%\\OtzariaVoice\\
speech_encoder on Windows, ~/.cache/otzaria-voice/speech_encoder elsewhere;
OTZARIA_SPEECH_ENCODER or --speech-encoder names it instead).

Needs: numpy, onnxruntime, soundfile (pip install numpy onnxruntime soundfile).
huggingface_hub is used for the download when installed; plain HTTPS otherwise.
"""
import argparse
import math
import os
import struct
import sys
import urllib.request
from pathlib import Path

import numpy as np

REPO = "onnx-community/chatterbox-multilingual-ONNX"
FILES = ("speech_encoder.onnx", "speech_encoder.onnx_data")
SAMPLE_RATE = 24000
PROMPT_SECONDS = 10          # the encoder keeps only this much of the recording
OUTPUT_NAMES = ("cond_emb", "prompt_token", "ref_x_vector", "prompt_feat")
TYPES = {np.dtype("float32"): 1, np.dtype("int64"): 7}   # ONNX element types


# ------------------------------------------------------------------ encoder

def default_cache_dir() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
        return Path(base) / "OtzariaVoice" / "speech_encoder"
    return Path.home() / ".cache" / "otzaria-voice" / "speech_encoder"


def download(url: str, dst: Path) -> None:
    """Plain HTTPS download to dst via a .part file, with a progress line."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    part = dst.with_suffix(dst.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "otzaria-voice make_voice_from_wav"})
    with urllib.request.urlopen(req) as r, open(part, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            if total:
                print(f"\r  {dst.name}: {got / 1e6:.0f}/{total / 1e6:.0f} MB", end="", flush=True)
        print()
    if total and got != total:
        part.unlink(missing_ok=True)
        raise RuntimeError(f"{url}: got {got} of {total} bytes")
    part.replace(dst)


def fetch_encoder(cache: Path) -> Path:
    """Returns the path of speech_encoder.onnx, downloading both files if needed."""
    onnx_dir = cache / "onnx"
    model = onnx_dir / FILES[0]
    missing = [f for f in FILES if not (onnx_dir / f).exists()]
    if not missing:
        return model
    print(f"Downloading the speech encoder (about 590 MB) into {cache}")
    try:
        from huggingface_hub import hf_hub_download
    except ImportError:
        hf_hub_download = None
    for f in missing:
        if hf_hub_download:
            hf_hub_download(REPO, f, subfolder="onnx", local_dir=str(cache))
        else:
            download(f"https://huggingface.co/{REPO}/resolve/main/onnx/{f}", onnx_dir / f)
    return model


def resolve_encoder(arg):
    """--speech-encoder may be the .onnx file, its folder, or the cache folder."""
    arg = arg or os.environ.get("OTZARIA_SPEECH_ENCODER")
    if not arg:
        return fetch_encoder(default_cache_dir())
    p = Path(arg)
    if p.is_file():
        return p
    for cand in (p / FILES[0], p / "onnx" / FILES[0]):
        if cand.exists():
            return cand
    return fetch_encoder(p)


# -------------------------------------------------------------------- audio

def resample(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """Windowed-sinc resampling (float32 mono), done in chunks; numpy only."""
    if sr_in == sr_out:
        return x.astype(np.float32)
    ratio = sr_in / sr_out                      # input samples per output sample
    half = 32                                   # kernel half-width in input samples
    cutoff = min(1.0, 1.0 / ratio) * 0.97       # fraction of the input Nyquist
    n_out = int(math.floor(len(x) / ratio))
    pad = np.concatenate([np.zeros(half, np.float64), x.astype(np.float64), np.zeros(half + 2, np.float64)])
    k = np.arange(-half, half + 1, dtype=np.float64)
    out = np.empty(n_out, np.float32)
    step = 1 << 15
    for start in range(0, n_out, step):
        n = np.arange(start, min(start + step, n_out), dtype=np.float64)
        t = n * ratio
        base = np.floor(t).astype(np.int64)
        frac = (t - base)[:, None]
        # Kernel for each output sample: sinc low-pass at the cutoff, Hann window.
        arg = k[None, :] - frac
        h = cutoff * np.sinc(cutoff * arg) * (0.5 + 0.5 * np.cos(np.pi * arg / (half + 1)))
        h /= h.sum(axis=1, keepdims=True)
        idx = base[:, None] + half + k[None, :].astype(np.int64)
        out[start:start + len(n)] = (pad[idx] * h).sum(axis=1)
    return out


def load_audio(path: Path) -> np.ndarray:
    import soundfile as sf
    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    mono = data.mean(axis=1)
    print(f"Read {path.name}: {len(mono) / sr:.1f} s, {sr} Hz, {data.shape[1]} channel(s)")
    return resample(mono, sr, SAMPLE_RATE)


def trim_silence(x: np.ndarray, db: float = -40.0, frame: int = 480) -> np.ndarray:
    """Cuts leading and trailing stretches quieter than db (relative to peak)."""
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak == 0.0:
        raise SystemExit("the recording is silent")
    n = len(x) // frame
    rms = np.sqrt((x[: n * frame].reshape(n, frame) ** 2).mean(axis=1))
    loud = np.flatnonzero(20 * np.log10(rms / peak + 1e-12) > db)
    if len(loud) == 0:
        return x
    a = max(0, (loud[0] - 5) * frame)                 # keep ~0.1 s of lead-in
    b = min(len(x), (loud[-1] + 6) * frame)
    return x[a:b]


# -------------------------------------------------------------------- write

def write_otzvoice(dst: Path, arrays: dict) -> None:
    """Same format make_voice.py writes and Voice::load reads."""
    with open(dst, "wb") as f:
        f.write(b"OTZVOICE" + struct.pack("<II", 1, len(OUTPUT_NAMES)))
        for name in OUTPUT_NAMES:
            a = np.ascontiguousarray(arrays[name])
            f.write(struct.pack("<I", len(name)) + name.encode())
            f.write(struct.pack("<II", TYPES[a.dtype], a.ndim))
            f.write(struct.pack(f"<{a.ndim}q", *a.shape))
            f.write(a.astype(a.dtype.newbyteorder("<")).tobytes())


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("recording", type=Path, help="WAV/FLAC/OGG of one speaker, 10-30 s")
    ap.add_argument("out", type=Path, help="the .otzvoice file to write")
    ap.add_argument("--speech-encoder", metavar="PATH",
                    help="speech_encoder.onnx, its folder, or the cache folder to download into")
    ap.add_argument("--start", type=float, default=0.0, metavar="SEC",
                    help="skip this many seconds of the recording first (default 0)")
    ap.add_argument("--no-trim", action="store_true", help="keep leading/trailing silence")
    args = ap.parse_args()

    audio = load_audio(args.recording)
    if args.start > 0:
        audio = audio[int(args.start * SAMPLE_RATE):]
    if not args.no_trim:
        audio = trim_silence(audio)
    peak = float(np.max(np.abs(audio)))
    if peak > 1.0:
        audio = audio / peak * 0.95
    elif peak < 0.05:
        print("warning: the recording is very quiet; consider a louder one", file=sys.stderr)
    seconds = len(audio) / SAMPLE_RATE
    if seconds < 3:
        raise SystemExit(f"only {seconds:.1f} s of speech; record at least 5 seconds (10 is best)")
    if seconds < 8:
        print(f"warning: only {seconds:.1f} s of speech; {PROMPT_SECONDS} s gives a better voice", file=sys.stderr)
    # The encoder keeps the first 10 s of what it gets; cut a little past that
    # so the window is not padded, and never feed it minutes of audio.
    audio = audio[: int((PROMPT_SECONDS + 1) * SAMPLE_RATE)]

    import onnxruntime as ort
    model = resolve_encoder(args.speech_encoder)
    print(f"Speech encoder: {model}")
    session = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    print(f"Encoding {min(seconds, PROMPT_SECONDS):.1f} s of speech ...")
    outputs = session.run(None, {"audio_values": audio[np.newaxis, :].astype(np.float32)})
    arrays = {
        "cond_emb": outputs[0].astype(np.float32),
        "prompt_token": outputs[1].astype(np.int64),
        "ref_x_vector": outputs[2].astype(np.float32),
        "prompt_feat": outputs[3].astype(np.float32),
    }
    for name in OUTPUT_NAMES:
        print(f"  {name}: {arrays[name].dtype} {list(arrays[name].shape)}")
    if arrays["ref_x_vector"].shape != (1, 192) or arrays["cond_emb"].ndim != 3 or arrays["prompt_feat"].ndim != 3:
        raise SystemExit("unexpected encoder output shapes; is this the Chatterbox Multilingual speech encoder?")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_otzvoice(args.out, arrays)
    print(f"Wrote {args.out} ({args.out.stat().st_size / 1e3:.0f} KB)")
    print("Install it: copy to %APPDATA%\\otzaria\\tts\\voices\\<id>.otzvoice (Windows) and restart the service;"
          " the id is the file name without the extension.")


if __name__ == "__main__":
    main()
