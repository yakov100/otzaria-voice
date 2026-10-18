#!/bin/bash
# Piper training environment in a clean venv (the image's site-packages conflicted with the [train] extras).
set -euo pipefail
cd /workspace
rm -rf /workspace/venv-piper
python3 -m venv /workspace/venv-piper
source /workspace/venv-piper/bin/activate
pip install -q -U pip wheel
pip install -q torch torchaudio --index-url https://download.pytorch.org/whl/cu124
cd /workspace/piper1-gpl
pip install -e '.[train]' 2>&1 | tail -3
./build_monotonic_align.sh
pip install -q soundfile huggingface_hub onnxruntime
python -c "import torch, piper, lightning; print('piper env ok; torch', torch.__version__, 'cuda', torch.cuda.is_available(), 'lightning', lightning.__version__)"
python -m piper.train fit --help | head -3
echo "setup2 done"
