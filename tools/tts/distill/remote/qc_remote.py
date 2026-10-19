"""Quality control of generated clips on a Linux GPU machine: the same checks
as qc.py (character error rate against the text, speed, inner silence,
clipping) with a Hebrew Whisper (ivrit-ai, through faster-whisper) as the
recognizer instead of the Windows service's --eval.

    source /workspace/venv-cb/bin/activate   # or any env with torch + CUDA
    pip install faster-whisper soundfile numpy
    python qc_remote.py --work /workspace/gen [--model ivrit-ai/whisper-large-v3-turbo-ct2] [--max-cer 0.15]

<work> holds clips_*.zip (from generate_remote.py) and/or wavs/<id>.wav with
clips.jsonl; the zips are unpacked into wavs/ first. Output as qc.py:
metadata.csv (NFD text, passing clips), qc.json, rejected.jsonl, asr.jsonl.
The CER threshold was calibrated on the Windows recognizer; Whisper spells a
little differently, so check qc.json's per-source medians before trusting it.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys
import unicodedata
import zipfile

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from qc import cer, plain, skeleton, sound_key, audio_stats  # noqa: E402


def unpack(work):
    wavs = os.path.join(work, "wavs")
    os.makedirs(wavs, exist_ok=True)
    clips_path = os.path.join(work, "clips.jsonl")
    have = set()
    if os.path.exists(clips_path):
        with open(clips_path, encoding="utf-8") as f:
            have = {json.loads(l)["id"] for l in f if l.strip()}
    with open(clips_path, "a", encoding="utf-8") as out:
        for z in sorted(glob.glob(os.path.join(work, "clips_*.zip"))):
            with zipfile.ZipFile(z) as zf:
                for line in zf.read("clips.jsonl").decode("utf-8").splitlines():
                    if not line.strip():
                        continue
                    row = json.loads(line)
                    if row["id"] in have:
                        continue
                    with open(os.path.join(wavs, row["id"] + ".wav"), "wb") as f:
                        f.write(zf.read(row["id"] + ".wav"))
                    out.write(json.dumps(row, ensure_ascii=False) + "\n")
                    have.add(row["id"])
    clips = []
    with open(clips_path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                clips.append(json.loads(line))
    return clips


def transcribe(work, clips, model_name, device, shard=None):
    """asr.jsonl: {"id", "hyp"} for every clip (resumes). With shard (k, n) only
    every n-th clip from the k-th, into asr.k.jsonl (one process a GPU); a run
    without shard reads all of them."""
    out_path = os.path.join(work, "asr.jsonl" if shard is None else f"asr.{shard[0]}.jsonl")
    done = {}
    for path in [os.path.join(work, "asr.jsonl")] + sorted(glob.glob(os.path.join(work, "asr.*.jsonl"))):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    if line.strip():
                        try:
                            row = json.loads(line)
                        except json.JSONDecodeError:  # a line cut by a kill
                            continue
                        done[row["id"]] = row["hyp"]
    if shard is not None:
        clips = clips[shard[0]::shard[1]]
    todo = [c for c in clips if c["id"] not in done]
    if todo:
        from faster_whisper import WhisperModel
        model = WhisperModel(model_name, device=device, compute_type="float16" if device == "cuda" else "int8")
        with open(out_path, "a", encoding="utf-8") as f:
            for i, c in enumerate(todo, 1):
                wav = os.path.join(work, "wavs", c["id"] + ".wav")
                segments, _ = model.transcribe(wav, language="he", beam_size=1, vad_filter=False,
                                               condition_on_previous_text=False)
                hyp = " ".join(s.text.strip() for s in segments)
                done[c["id"]] = hyp
                f.write(json.dumps({"id": c["id"], "hyp": hyp}, ensure_ascii=False) + "\n")
                if i % 100 == 0:
                    print(f"asr {i}/{len(todo)}", flush=True)
    return done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="/workspace/gen")
    ap.add_argument("--model", default="ivrit-ai/whisper-large-v3-turbo-ct2")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--max-cer", type=float, default=0.15)
    ap.add_argument("--unpack-only", action="store_true", help="unpack the zips and stop (before --shard runs)")
    ap.add_argument("--shard", default=None, help="k/n: transcribe only this share, then stop")
    args = ap.parse_args()
    work = args.work
    if args.shard:  # the zips are unpacked already (--unpack-only)
        clips = [json.loads(l) for l in open(os.path.join(work, "clips.jsonl"), encoding="utf-8") if l.strip()]
        k, n = (int(x) for x in args.shard.split("/"))
        transcribe(work, clips, args.model, args.device, (k, n))
        return
    clips = unpack(work)
    print(len(clips), "clips", flush=True)
    if args.unpack_only:
        return
    hyps = transcribe(work, clips, args.model, args.device)

    passed, rejected = [], []
    per = collections.defaultdict(lambda: {"clips": 0, "passed": 0, "seconds": 0.0, "cer": []})
    for c in clips:
        stats = audio_stats(os.path.join(work, "wavs", c["id"] + ".wav"))
        ref = plain(c["text"])
        letters = len(ref.replace(" ", ""))
        reasons = []
        hyp = plain(hyps.get(c["id"], ""))
        c["cer_raw"] = round(cer(ref, hyp), 3)
        c["cer"] = round(min(c["cer_raw"], cer(skeleton(ref), skeleton(hyp)), cer(sound_key(ref), sound_key(hyp))), 3)
        if c["cer"] > args.max_cer:
            reasons.append(f"cer {c['cer']}")
        speed = letters / max(stats["seconds"], 0.01)
        c.update(stats)
        c["letters_per_second"] = round(speed, 1)
        if not 6 <= speed <= 18:
            reasons.append(f"speed {speed:.1f}")
        if stats["silence"] > 1.5:
            reasons.append(f"silence {stats['silence']:.1f}s")
        if stats["clipping"] > 0.001:
            reasons.append(f"clipping {stats['clipping']:.4f}")
        p = per[c["source"]]
        p["clips"] += 1
        p["seconds"] += stats["seconds"]
        p["cer"].append(c["cer"])
        if reasons:
            c["reasons"] = reasons
            rejected.append(c)
        else:
            p["passed"] += 1
            passed.append(c)

    with open(os.path.join(work, "metadata.csv"), "w", encoding="utf-8") as f:
        for c in passed:
            f.write(f"{c['id']}.wav|{unicodedata.normalize('NFD', c['text'].replace('|', ''))}\n")
    with open(os.path.join(work, "rejected.jsonl"), "w", encoding="utf-8") as f:
        for c in rejected:
            f.write(json.dumps(c, ensure_ascii=False) + "\n")
    summary = {"clips": len(clips), "passed": len(passed), "hours_passed": sum(c["seconds"] for c in passed) / 3600,
               "per_source": {}, "recognizer": args.model}
    for s, p in per.items():
        cers = sorted(p["cer"])
        summary["per_source"][s] = {"clips": p["clips"], "passed": p["passed"], "hours": p["seconds"] / 3600,
                                    "cer_median": cers[len(cers) // 2] if cers else None,
                                    "cer_p90": cers[int(len(cers) * 0.9)] if cers else None}
    summary["reject_reasons"] = collections.Counter(r.split()[0] for c in rejected for r in c["reasons"])
    with open(os.path.join(work, "qc.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=1)
    print(json.dumps(summary, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
