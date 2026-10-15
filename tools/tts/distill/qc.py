"""Checks the clips and writes the training list.

    python qc.py [--work ...] [--exe ...] [--skip-asr] [--max-cer 0.12]

For every clip in <work>/clips.jsonl:
  * the service's recognizer transcribes the 16 kHz copy (`--eval`, second
    pass off) and the character error rate against the text is measured,
    on the text as is and on its "skeleton" (without vav/yod, since the
    recognizer may spell fuller or leaner);
  * speed (letters a second), the longest inner silence and clipping are
    measured on the audio.
A clip passes with CER <= --max-cer, 6-18 letters a second, no silence over
1.5 s inside, and under 0.1% clipped samples. Output:
  <work>/metadata.csv   id.wav|text (NFD) for piper (passing clips only)
  <work>/qc.json        counts and distributions per source
  <work>/rejected.jsonl the failed clips with their reasons
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import unicodedata

import numpy as np
import soundfile as sf

NIKUD_MARKS = re.compile("[֑-ׇ|]")
PUNCT = re.compile("[^א-ת ]")


def cer(ref, hyp):
    if not ref:
        return 1.0
    prev = list(range(len(hyp) + 1))
    for i, a in enumerate(ref, 1):
        cur = [i]
        for j, b in enumerate(hyp, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a != b)))
        prev = cur
    return prev[-1] / len(ref)


def plain(text):
    return re.sub(r"\s+", " ", PUNCT.sub(" ", NIKUD_MARKS.sub("", text))).strip()


def skeleton(text):
    return re.sub(r"\s+", " ", text.replace("ו", "").replace("י", "")).strip()


# Letters that sound alike in Israeli Hebrew fold together, and the silent
# ones go: the recognizer spells by its dictionary, not by sound (קונן ->
# קונה, טפוחים -> תפוחים), and such spellings must not count as errors.
SOUND = str.maketrans({"א": "", "ע": "", "ה": "", "ט": "ת", "ק": "כ", "ך": "כ", "ח": "כ", "ש": "ס", "ץ": "צ",
                       "ף": "פ", "ם": "מ", "ן": "נ", "ב": "ו"})


def sound_key(text):
    return re.sub(r"\s+", " ", skeleton(text).translate(SOUND)).strip()


def audio_stats(path):
    x, rate = sf.read(path, dtype="float32")
    if x.ndim > 1:
        x = x.mean(axis=1)
    frame = int(rate * 0.02)
    n = max(1, len(x) // frame)
    rms = np.sqrt((x[:n * frame].reshape(n, frame) ** 2).mean(axis=1) + 1e-12)
    quiet = 20 * np.log10(rms) < -40
    longest = run = 0
    for q in quiet:
        run = run + 1 if q else 0
        longest = max(longest, run)
    return {"seconds": len(x) / rate, "silence": longest * frame / rate,
            "clipping": float((np.abs(x) >= 0.99).mean())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "distill"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--skip-asr", action="store_true")
    ap.add_argument("--reuse-asr", action="store_true", help="score the transcripts already in asr.jsonl")
    ap.add_argument("--max-cer", type=float, default=0.12)
    args = ap.parse_args()
    work = args.work
    exe = args.exe or os.path.join(work, "otzaria-voice.exe")
    clips = []
    with open(os.path.join(work, "clips.jsonl"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                clips.append(json.loads(line))

    hyps = {}
    if not args.skip_asr:
        qc_dir = os.path.join(work, "wav16")
        manifest = os.path.join(qc_dir, "manifest.jsonl")
        out = os.path.join(work, "asr.jsonl")
        if not args.reuse_asr:
            with open(manifest, "w", encoding="utf-8", newline="\n") as f:
                for c in clips:
                    f.write(json.dumps({"id": c["id"], "wav": c["id"] + ".wav", "ref": plain(c["text"]),
                                        "tags": [c["source"]], "split": "qc"}, ensure_ascii=False) + "\n")
            env = dict(os.environ, OTZARIA_VOICE_NO_RESCORE="1")
            r = subprocess.run([exe, "--eval", manifest, out], env=env)
            if r.returncode != 0:
                sys.exit("eval failed: " + str(r.returncode))
        with open(out, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    row = json.loads(line)
                    hyps[row["id"]] = row.get("hyp", "")

    passed, rejected = [], []
    per = collections.defaultdict(lambda: {"clips": 0, "passed": 0, "seconds": 0.0, "cer": []})
    for c in clips:
        stats = audio_stats(os.path.join(work, "wav16", c["id"] + ".wav"))
        ref = plain(c["text"])
        letters = len(ref.replace(" ", ""))
        reasons = []
        if hyps:
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
        if "cer" in c:
            p["cer"].append(c["cer"])
        if reasons:
            c["reasons"] = reasons
            rejected.append(c)
        else:
            p["passed"] += 1
            passed.append(c)

    with open(os.path.join(work, "metadata.csv"), "w", encoding="utf-8", newline="\n") as f:
        for c in passed:
            text = unicodedata.normalize("NFD", c["text"].replace("|", ""))
            f.write(f"{c['id']}.wav|{text}\n")
    with open(os.path.join(work, "rejected.jsonl"), "w", encoding="utf-8", newline="\n") as f:
        for c in rejected:
            f.write(json.dumps(c, ensure_ascii=False) + "\n")
    summary = {"clips": len(clips), "passed": len(passed), "hours_passed": sum(c["seconds"] for c in passed) / 3600,
               "per_source": {}}
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
