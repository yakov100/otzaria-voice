#!/bin/bash
# Piper's training code on a rented GPU machine (a PyTorch/CUDA image): a venv
# that sees the image's torch, the [train] extras, the monotonic-align build.
# PyPI itself crawled from the Vast.ai host (45 KB/s); a mirror ran at 1 MB/s,
# so PIP_INDEX picks one. Idempotent.
#   bash setup_piper.sh [https://pypi.tuna.tsinghua.edu.cn/simple]
set -euo pipefail
PIP_INDEX=${1:-https://pypi.tuna.tsinghua.edu.cn/simple}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -qq -y build-essential cmake ninja-build git unzip python3-venv libsndfile1 >/dev/null
mkdir -p /workspace/{data,runs,gen-input,gen-output,models,tools}
cd /workspace
[ -d piper1-gpl ] || git clone -q https://github.com/OHF-Voice/piper1-gpl.git
[ -d venv-sys ] || python3 -m venv --system-site-packages /workspace/venv-sys
source /workspace/venv-sys/bin/activate
cd /workspace/piper1-gpl
for try in 1 2 3; do pip install -i "$PIP_INDEX" --timeout 120 -e '.[train]' > /workspace/pip_train.log 2>&1 && break; echo "pip attempt $try failed"; sleep 10; done
./build_monotonic_align.sh 2>&1 | tail -1
pip install -q -i "$PIP_INDEX" --timeout 120 soundfile huggingface_hub onnx onnxruntime
python -c "import torch, piper, lightning; print('piper env ok; torch', torch.__version__, 'cuda', torch.cuda.is_available(), 'lightning', lightning.__version__)"
echo "setup done"
