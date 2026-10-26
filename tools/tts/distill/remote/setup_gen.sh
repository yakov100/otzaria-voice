#!/bin/bash
# A generator machine (Vast.ai, PyTorch/CUDA image): Chatterbox in a venv that
# sees the image's torch 2.6 (chatterbox-tts pins torch 2.6), from a PyPI mirror
# (PyPI itself crawled from Vast hosts). Idempotent.
#   bash setup_gen.sh [pip index url]
set -euo pipefail
PIP_INDEX=${1:-https://pypi.tuna.tsinghua.edu.cn/simple}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -qq -y libsndfile1 python3-venv >/dev/null
mkdir -p /workspace/{gen-input,gen-output,tools}
[ -d /workspace/venv-cb ] || python3 -m venv --system-site-packages /workspace/venv-cb
source /workspace/venv-cb/bin/activate
pip install -q -U pip >/dev/null 2>&1 || true
for try in 1 2 3; do
  pip install -i "$PIP_INDEX" --timeout 120 chatterbox-tts soundfile > /workspace/pip_cb.log 2>&1 && break
  echo "pip attempt $try failed"; tail -2 /workspace/pip_cb.log; sleep 10
done
python - <<'EOF'
import torch, torchaudio
print("torch", torch.__version__, "torchaudio", torchaudio.__version__, "cuda", torch.cuda.is_available())
from chatterbox.mtl_tts import ChatterboxMultilingualTTS
print("chatterbox import ok")
EOF
nvidia-smi -L
echo "SETUP_GEN_DONE"
