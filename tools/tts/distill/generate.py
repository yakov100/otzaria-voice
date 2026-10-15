"""Reads the selected sentences in the recorded voice with the heavy model
(Chatterbox through `otzaria-voice.exe --narrate`), in shards, and turns
each line into a training clip: peak at 0.89, silence trimmed, 22.05 kHz
(for the light voice) and 16 kHz (for the recognizer's check).

    python generate.py --work %LOCALAPPDATA%\\OtzariaVoice\\distill
        [--exe otzaria-voice.exe] [--pack %APPDATA%\\otzaria\\tts\\pack]
        [--voice <pack>\\voices\\chaim.otzvoice] [--shard 300] [--limit-lines N]
        [--prompt-seconds 5]

Input (from select_text.py and `--vocalize --marks`):
  <work>/text/sentences.tsv            id, source, book, vocalized, text
  <work>/text/sentences.vocalized.txt  the same lines with nikud and marks
Output:
  <work>/gen/NNNN/               the narrator's WAVs and report for a shard (+ `done`)
  <work>/wavs/<id>.wav           22.05 kHz mono 16-bit clip
  <work>/wav16/<id>.wav          16 kHz copy for qc.py
  <work>/clips.jsonl             id, source, book, text (with marks), seconds, ms

Resumes: shards with a `done` file are skipped. A file named STOP in <work>
ends the run after the current shard (run_nights.ps1 writes it at morning).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

MARKS = re.compile("[ֽ֫|]")  # the pronunciation marks: not for the heavy model
RATE_OUT, RATE_QC = 22050, 16000


def trim_silence(x, rate, threshold_db=-40.0, pad_ms=60):
    """Cuts leading and trailing silence, keeping pad_ms of it."""
    frame = int(rate * 0.01)
    if len(x) < frame * 3:
        return x
    n = len(x) // frame
    rms = np.sqrt((x[:n * frame].reshape(n, frame) ** 2).mean(axis=1) + 1e-12)
    loud = np.where(20 * np.log10(rms) > threshold_db)[0]
    if len(loud) == 0:
        return x
    pad = int(rate * pad_ms / 1000)
    a = max(0, loud[0] * frame - pad)
    b = min(len(x), (loud[-1] + 1) * frame + pad)
    return x[a:b]


def process_clip(src_path, dst22, dst16):
    x, rate = sf.read(src_path, dtype="float32", always_2d=False)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x = trim_silence(x, rate)
    peak = float(np.abs(x).max()) if len(x) else 0.0
    if peak > 0:
        x = x * (0.89 / peak)
    y22 = resample_poly(x, RATE_OUT, rate) if rate != RATE_OUT else x
    y16 = resample_poly(x, RATE_QC, rate) if rate != RATE_QC else x
    sf.write(dst22, np.clip(y22, -1, 1), RATE_OUT, subtype="PCM_16")
    sf.write(dst16, np.clip(y16, -1, 1), RATE_QC, subtype="PCM_16")
    return len(y22) / RATE_OUT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--pack", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "tts", "pack"))
    ap.add_argument("--voice", default=None)
    ap.add_argument("--shard", type=int, default=300)
    ap.add_argument("--limit-lines", type=int, default=0, help="stop after this many lines (0 = all)")
    ap.add_argument("--prompt-seconds", type=float, default=5)
    ap.add_argument("--lm-threads", type=int, default=5)
    ap.add_argument("--decoder-threads", type=int, default=6)
    args = ap.parse_args()
    work = args.work
    exe = args.exe or os.path.join(work, "otzaria-voice.exe")
    voice = args.voice or os.path.join(args.pack, "voices", "chaim.otzvoice")
    for p, what in [(exe, "service exe"), (voice, "voice file")]:
        if not os.path.exists(p):
            sys.exit(f"missing {what}: {p}")

    rows = []
    with open(os.path.join(work, "text", "sentences.tsv"), encoding="utf-8") as tsv, \
            open(os.path.join(work, "text", "sentences.vocalized.txt"), encoding="utf-8") as voc:
        next(tsv)
        for meta, text in zip(tsv, voc):
            sid, source, book, vocalized, _ = meta.rstrip("\n").split("\t", 4)
            rows.append((sid, source, book, text.rstrip("\n")))
    if args.limit_lines:
        rows = rows[:args.limit_lines]
    for d in ("gen", "wavs", "wav16"):
        os.makedirs(os.path.join(work, d), exist_ok=True)
    clips_path = os.path.join(work, "clips.jsonl")
    done_ids = set()
    if os.path.exists(clips_path):
        with open(clips_path, encoding="utf-8") as f:
            done_ids = {json.loads(l)["id"] for l in f if l.strip()}

    env = dict(os.environ,
               OTZARIA_NARRATOR_PIPELINE="1",
               OTZARIA_NARRATOR_PROMPT_SECONDS=str(args.prompt_seconds),
               OTZARIA_NARRATOR_LM_THREADS=str(args.lm_threads),
               OTZARIA_NARRATOR_DECODER_THREADS=str(args.decoder_threads),
               OTZARIA_NARRATOR_BATCH="6",
               OTZARIA_NARRATOR_TIMING="1")  # per-batch lines in service.log: where a stall is
    shards = [rows[i:i + args.shard] for i in range(0, len(rows), args.shard)]
    for n, shard in enumerate(shards):
        # Lines already in clips.jsonl are never redone, whatever the shard
        # size was when they were made.
        shard = [r for r in shard if r[0] not in done_ids]
        if not shard:
            continue
        shard_dir = os.path.join(work, "gen", f"{n:04d}")
        if os.path.exists(os.path.join(work, "STOP")):
            print("STOP found; stopping before shard", n, file=sys.stderr)
            return
        os.makedirs(shard_dir, exist_ok=True)
        text_path = os.path.join(shard_dir, "shard.txt")
        with open(text_path, "w", encoding="utf-8", newline="\n") as f:
            for sid, source, book, text in shard:
                f.write(MARKS.sub("", text) + "\n")
        t0 = time.time()
        print(f"shard {n + 1}/{len(shards)}: {len(shard)} lines ...", file=sys.stderr, flush=True)
        # Below-normal priority: the computer stays usable while this runs.
        below_normal = 0x4000 if os.name == "nt" else 0
        r = subprocess.run([exe, "--narrate", args.pack, voice, text_path, shard_dir], env=env,
                           creationflags=below_normal)
        if r.returncode != 0:
            sys.exit(f"narrate failed on shard {n} (exit {r.returncode}); see %LOCALAPPDATA%\\OtzariaVoice\\service.log")
        report = {}
        with open(os.path.join(shard_dir, "report.jsonl"), encoding="utf-8") as f:
            for line in f:
                row = json.loads(line)
                if "line" in row:
                    report[row["line"]] = row
        with open(clips_path, "a", encoding="utf-8", newline="\n") as out:
            for i, (sid, source, book, text) in enumerate(shard, 1):
                src = os.path.join(shard_dir, f"{i:02d}.wav")
                if not os.path.exists(src) or i not in report:
                    continue
                seconds = process_clip(src, os.path.join(work, "wavs", sid + ".wav"),
                                       os.path.join(work, "wav16", sid + ".wav"))
                out.write(json.dumps({"id": sid, "source": source, "book": book, "text": text,
                                      "seconds": round(seconds, 2), "ms": report[i].get("ms", 0)},
                                     ensure_ascii=False) + "\n")
        for i in range(1, len(shard) + 1):  # the raw 24 kHz files are not needed any more
            try:
                os.remove(os.path.join(shard_dir, f"{i:02d}.wav"))
            except OSError:
                pass
        done_ids.update(r[0] for r in shard)
        audio = sum(report[i]["seconds"] for i in report)
        print(f"  {audio / 60:.1f} min of audio in {(time.time() - t0) / 60:.1f} min", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
