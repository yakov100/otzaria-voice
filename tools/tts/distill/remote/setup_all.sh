#!/bin/bash
# The machine that generates, checks and trains (one with several GPUs):
# Chatterbox, Piper with the multi-GPU fix, and every model downloaded once
# before many processes ask for it at the same moment. Idempotent.
#   nohup bash setup_all.sh > /workspace/setup_all.log 2>&1 &
set -u
cd /workspace
bash tools/setup_gen.sh || { echo SETUP_FAILED gen; exit 1; }
bash tools/setup_piper.sh || { echo SETUP_FAILED piper; exit 1; }
# Piper under DDP: prepare_data runs only on rank 0, so the others had no config.
python3 - <<'PY'
p = "/workspace/piper1-gpl/src/piper/train/vits/dataset.py"
s = open(p).read()
old = "    def setup(self, stage: str) -> None:\n        assert self.piper_config is not None\n"
new = ("    def setup(self, stage: str) -> None:\n"
       "        if self.piper_config is None:  # DDP: prepare_data ran only on rank 0 (the cache is ready)\n"
       "            self.prepare_data()\n"
       "        assert self.piper_config is not None\n")
if old in s:
    open(p, "w").write(s.replace(old, new)); print("piper: DDP fix applied")
else:
    print("piper: DDP fix already there" if "DDP: prepare_data" in s else "piper: setup() changed, check by hand")
PY
source /workspace/venv-sys/bin/activate
# UTMOS (val_mos): four ranks downloading it at once broke it on some of them.
(cd /tmp && python -c "import torch; torch.hub.load('tarepan/SpeechMOS:v1.2.0', 'utmos22_strong', trust_repo=True); print('utmos cached')")
pip install -q -i https://pypi.tuna.tsinghua.edu.cn/simple --timeout 120 faster-whisper > /dev/null 2>&1
python -c "from huggingface_hub import snapshot_download; snapshot_download('ivrit-ai/whisper-large-v3-turbo-ct2'); print('whisper cached')"
[ -e /workspace/venv-qc ] || ln -s /workspace/venv-sys /workspace/venv-qc
source /workspace/venv-cb/bin/activate
OMP_NUM_THREADS=6 python tools/generate_remote.py --load-only 2>&1 | tail -1
echo SETUP_ALL_DONE
