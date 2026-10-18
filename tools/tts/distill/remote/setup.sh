#!/bin/bash
# Prepares a rented GPU machine (Vast.ai, a PyTorch/CUDA image, Ubuntu) for the
# light-voice work: Piper's training code in one virtual environment and
# Chatterbox (the recorded voice, for generating training audio) in another,
# since they pin different PyTorch versions. Idempotent; run as root.
#
#   bash setup.sh            # both
#   bash setup.sh piper      # training only
#   bash setup.sh chatterbox # generation only
set -euo pipefail
WANT=${1:-all}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -qq -y build-essential cmake ninja-build git unzip python3-venv libsndfile1 >/dev/null
mkdir -p /workspace/{data,runs,gen-input,gen-output,models}
cd /workspace
nvidia-smi -L || echo "no GPU visible"

if [ "$WANT" = all ] || [ "$WANT" = piper ]; then
  if [ ! -d /workspace/piper1-gpl ]; then git clone -q https://github.com/OHF-Voice/piper1-gpl.git; fi
  if [ ! -d /workspace/venv-piper ]; then
    python3 -m venv --system-site-packages /workspace/venv-piper   # keeps the image's CUDA torch
  fi
  # shellcheck disable=SC1091
  source /workspace/venv-piper/bin/activate
  pip install -q -U pip
  (cd /workspace/piper1-gpl && pip install -q -e '.[train]' && ./build_monotonic_align.sh)
  pip install -q soundfile huggingface_hub onnx onnxruntime
  python -c "import torch, piper; print('piper env ok; torch', torch.__version__, 'cuda', torch.cuda.is_available())"
  deactivate
fi

if [ "$WANT" = all ] || [ "$WANT" = chatterbox ]; then
  if [ ! -d /workspace/venv-cb ]; then python3 -m venv /workspace/venv-cb; fi
  # shellcheck disable=SC1091
  source /workspace/venv-cb/bin/activate
  pip install -q -U pip
  pip install -q chatterbox-tts soundfile
  python -c "import torch; print('chatterbox env ok; torch', torch.__version__, 'cuda', torch.cuda.is_available())"
  deactivate
fi
echo "setup done"
