#!/bin/bash
# On a generator machine, after generate_remote.py: quality control with the
# Hebrew Whisper (qc_remote.py) on this machine's own clips, then the passing
# clips are collected in /workspace/qc-out (wavs/ + metadata.csv + qc.json),
# ready for `vastai copy` to the training machine. Installs faster-whisper
# first (so a failure shows early), then waits for the generation to end.
#   nohup bash finish_gen.sh > /workspace/finish.log 2>&1 &
set -u
WORK=/workspace/gen-output; OUT=/workspace/qc-out
[ -d /workspace/venv-qc ] || python3 -m venv --system-site-packages /workspace/venv-qc
source /workspace/venv-qc/bin/activate
python -c "import faster_whisper" 2>/dev/null || for try in 1 2 3; do
  pip install -i https://pypi.tuna.tsinghua.edu.cn/simple --timeout 120 faster-whisper soundfile > /workspace/pip_fw.log 2>&1 && break
  echo "pip attempt $try failed"; sleep 10
done
python -c "import faster_whisper, ctranslate2; print('faster-whisper ok, cuda devices', ctranslate2.get_cuda_device_count())"
python -c "from huggingface_hub import snapshot_download; snapshot_download('ivrit-ai/whisper-large-v3-turbo-ct2')" && echo "whisper model cached"
while pgrep -f "generate_remote.py|gen_loop.sh" >/dev/null; do sleep 60; done
echo "generation ended $(date -u +%H:%M): $(grep -al "done" /workspace/gen*.log | wc -l) of $(ls /workspace/gen*.log | wc -l) logs say done"
# several processes per GPU (restart_parallel.sh) wrote into p2, p3, ...: gather their zips
for z in $WORK/*/clips_*.zip; do [ -e "$z" ] && mv "$z" "$WORK/clips_$(basename "$(dirname "$z")")_${z##*/clips_}"; done
# the transcription on every GPU at once (one shard each), then the verdicts
python /workspace/tools/qc_remote.py --work $WORK --unpack-only > /workspace/qc.log 2>&1 || { tail -5 /workspace/qc.log; echo QC_FAILED; exit 1; }
N=$(nvidia-smi -L | wc -l)
for k in $(seq 0 $((N-1))); do
  CUDA_VISIBLE_DEVICES=$k python /workspace/tools/qc_remote.py --work $WORK --shard $k/$N > /workspace/qc_$k.log 2>&1 &
done
wait
echo "asr on $N GPUs done $(date -u +%H:%M)"
python /workspace/tools/qc_remote.py --work $WORK >> /workspace/qc.log 2>&1 || { tail -5 /workspace/qc.log; echo QC_FAILED; exit 1; }
mkdir -p $OUT/wavs
cp $WORK/metadata.csv $WORK/qc.json $WORK/clips.jsonl $WORK/rejected.jsonl $OUT/
cut -d'|' -f1 $WORK/metadata.csv | while read f; do ln -f $WORK/wavs/$f $OUT/wavs/$f 2>/dev/null || cp $WORK/wavs/$f $OUT/wavs/; done
echo "FINISH_DONE $(wc -l < $OUT/metadata.csv) clips passed of $(wc -l < $WORK/clips.jsonl)"
