#!/bin/bash
# Restarts this machine's generators with a thread limit per process: on a
# host whose CPU quota is smaller than its core count (Vast: 32 of 255), every
# process opened a thread per core and they thrashed (load 160, 5 clips/min
# for the whole machine). A kill keeps the clips made so far (the SIGTERM
# handler writes them), and generate_remote.py skips ids in any zip here, so
# the ranges can be re-split freely. Process i writes into gen-output/q<tag>i.
#   nohup bash retune.sh <threads> <tag> "A B" "C D" ... > /workspace/retune.log 2>&1 &
set -u
T=$1; TAG=$2; shift 2
pkill -f "^bash /workspace/tools/finish_gen.sh"
pkill -f "python /workspace/tools/generate_remote.py"
for k in $(seq 30); do pgrep -f "python /workspace/tools/generate_remote.py" >/dev/null || break; sleep 2; done
pkill -9 -f "python /workspace/tools/generate_remote.py"; sleep 3
source /workspace/venv-cb/bin/activate; cd /workspace
export OMP_NUM_THREADS=$T MKL_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE
i=0
for r in "$@"; do
  i=$((i+1)); set -- $r
  out=/workspace/gen-output/q$TAG$i
  nohup python /workspace/tools/generate_remote.py --start $1 --end $2 --out $out > /workspace/gen$TAG$i.log 2>&1 < /dev/null &
  echo "process $i: lines $1-$2 -> $out, $T threads"
  sleep 20
done
nohup bash /workspace/tools/finish_gen.sh > /workspace/finish.log 2>&1 < /dev/null &
echo "RETUNED $i processes $(date -u +%H:%M)"
