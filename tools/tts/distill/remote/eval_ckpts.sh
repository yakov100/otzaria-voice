#!/bin/bash
# After the training: rank the saved checkpoints on text the model never saw.
# 1. evalset: the 10 listening sentences + 120 QC-rejected sentences (24 a source)
# 2. export every candidate checkpoint to ONNX on the CPU (in parallel)
# 3. render all 130 lines with each at 0.333/1.4/0.6 (CPU)
# 4. Whisper on the GPUs (free now), one candidate a GPU; letter error rate
set -u
W=/workspace/eval; mkdir -p $W/onnx $W/wav
source /workspace/venv-sys/bin/activate
python3 - <<'PY'
import json, random, collections
random.seed(7)
rows = [json.loads(l) for f in ("/workspace/qc-out/rejected.jsonl", "/workspace/gen/in2/no/rejected.jsonl") for l in open(f, encoding="utf-8") if l.strip()]
by = collections.defaultdict(list)
for r in rows:
    if 40 <= len(r["text"]) <= 220: by[r["source"]].append(r)
pick = []
for s in sorted(by): pick += random.sample(by[s], 24)
tests = [l.strip() for l in open("/workspace/gen/test_sentences.txt", encoding="utf-8") if l.strip()]
with open("/workspace/eval/evalset.jsonl", "w", encoding="utf-8") as f:
    for i, t in enumerate(tests): f.write(json.dumps({"k": f"t{i:02d}", "src": "test", "text": t}, ensure_ascii=False) + "\n")
    for r in pick: f.write(json.dumps({"k": r["id"], "src": r["source"], "text": r["text"].replace("|", "")}, ensure_ascii=False) + "\n")
print("evalset", len(tests) + len(pick), dict(collections.Counter(r["source"] for r in pick)))
PY
# candidates: the final state, the best by val_mel and by val_mos, and the last 500-step save
C=/workspace/lightning/lightning_logs/version_0/checkpoints
ls $C/*.ckpt /workspace/runs/full/ckpt/*.ckpt > $W/candidates.txt
cat $W/candidates.txt
export CUDA_VISIBLE_DEVICES="" OMP_NUM_THREADS=4
i=0
while read c; do
  i=$((i+1)); n=$(basename "$c" .ckpt | tr '=' '-')
  ( nice -n 10 python3 -m piper.train.export_onnx --checkpoint "$c" --output-file $W/onnx/$n.onnx > $W/onnx/$n.log 2>&1 && echo "exported $n" ) &
  [ $((i % 6)) -eq 0 ] && wait
done < $W/candidates.txt
wait
python3 - <<'PY'
import json, glob, os, unicodedata, wave, numpy as np, onnxruntime as ort
from concurrent.futures import ProcessPoolExecutor
cfg = json.load(open("/workspace/runs/full/chaim-light.json", encoding="utf-8")); idm = cfg["phoneme_id_map"]
ev = [json.loads(l) for l in open("/workspace/eval/evalset.jsonl", encoding="utf-8")]
def render(onnx):
    so = ort.SessionOptions(); so.intra_op_num_threads = 6
    s = ort.InferenceSession(onnx, so, providers=["CPUExecutionProvider"])
    d = "/workspace/eval/wav/" + os.path.basename(onnx)[:-5]; os.makedirs(d, exist_ok=True)
    for e in ev:
        ids = idm["^"] + idm["_"]
        for ch in unicodedata.normalize("NFD", e["text"]):
            if ch in idm: ids += idm[ch] + idm["_"]
        ids += idm["$"]
        x = np.array([ids], dtype=np.int64)
        a = s.run(None, {"input": x, "input_lengths": np.array([x.shape[1]], dtype=np.int64), "scales": np.array([0.333, 1.4, 0.6], dtype=np.float32)})[0].squeeze()
        w = wave.open(f"{d}/{e['k']}.wav", "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050); w.writeframes((np.clip(a, -1, 1) * 32767).astype(np.int16).tobytes()); w.close()
    return d
onnx = sorted(glob.glob("/workspace/eval/onnx/*.onnx"))
with ProcessPoolExecutor(8) as ex:
    for d in ex.map(render, onnx): print("rendered", os.path.basename(d))
PY
unset CUDA_VISIBLE_DEVICES
python3 - <<'PY'
import glob, json, os, subprocess, sys
dirs = sorted(glob.glob("/workspace/eval/wav/*"))
code = r'''
import sys, json, glob, os
sys.path.insert(0, "/workspace")
from qc import cer, plain, skeleton, sound_key
from faster_whisper import WhisperModel
m = WhisperModel("ivrit-ai/whisper-large-v3-turbo-ct2", device="cuda", compute_type="float16")
ev = {json.loads(l)["k"]: json.loads(l) for l in open("/workspace/eval/evalset.jsonl", encoding="utf-8")}
for d in sys.argv[1:]:
    out = {}
    for k, e in ev.items():
        segs, _ = m.transcribe(f"{d}/{k}.wav", language="he", beam_size=1, vad_filter=False, condition_on_previous_text=False)
        hyp = " ".join(s.text.strip() for s in segs); r, h = plain(e["text"]), plain(hyp)
        out[k] = {"src": e["src"], "cer": min(cer(r, h), cer(skeleton(r), skeleton(h)), cer(sound_key(r), sound_key(h))), "hyp": hyp}
    json.dump(out, open(d + ".asr.json", "w", encoding="utf-8"), ensure_ascii=False)
    print("asr", os.path.basename(d), flush=True)
'''
open("/workspace/eval/asr_worker.py", "w").write(code)
procs = []
for g in range(8):
    part = dirs[g::8]
    if part:
        env = dict(os.environ, CUDA_VISIBLE_DEVICES=str(g))
        procs.append(subprocess.Popen([sys.executable, "/workspace/eval/asr_worker.py"] + part, env=env))
for p in procs: p.wait()
rows = []
for d in dirs:
    a = json.load(open(d + ".asr.json", encoding="utf-8"))
    mean = lambda ks: sum(a[k]["cer"] for k in ks) / len(ks)
    test = [k for k in a if a[k]["src"] == "test"]; unseen = [k for k in a if a[k]["src"] != "test"]
    srcs = sorted(set(a[k]["src"] for k in unseen))
    rows.append((mean(unseen), os.path.basename(d), mean(test), {s: round(100 * mean([k for k in unseen if a[k]["src"] == s]), 1) for s in srcs}))
rows.sort()
for u, n, t, per in rows: print("%-36s unseen120 %.2f%%  test10 %.2f%%  %s" % (n, 100 * u, 100 * t, per))
json.dump(rows, open("/workspace/eval/ranking.json", "w"), ensure_ascii=False)
PY
echo EVAL_DONE
