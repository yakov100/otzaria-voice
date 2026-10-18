#!/bin/bash
# (Re)starts this machine's generation on explicit id lists (the leftovers,
# split evenly between all machines by rebalance.py): stops the running
# generators first (SIGTERM: they write what they made), then one process
# per list, round robin over the GPUs, each with a thread limit (unlimited
# threads thrash on a CPU quota), each into gen-output/r<i>. Then finish_gen.sh.
#   nohup bash gen_ids.sh <threads> <gpus> ids_1.txt ids_2.txt ... > /workspace/gen_ids.log 2>&1 &
set -u
T=$1; G=$2; shift 2
pkill -f "^bash /workspace/tools/finish_gen.sh"
pkill -f "python /workspace/tools/generate_remote.py"
for k in $(seq 30); do pgrep -f "python /workspace/tools/generate_remote.py" >/dev/null || break; sleep 2; done
pkill -9 -f "python /workspace/tools/generate_remote.py"; sleep 2
source /workspace/venv-cb/bin/activate; cd /workspace
export OMP_NUM_THREADS=$T MKL_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE
i=0
for f in "$@"; do
  gpu=$((i % G)); i=$((i+1))
  CUDA_VISIBLE_DEVICES=$gpu nohup python /workspace/tools/generate_remote.py --ids-file $f --end -1 \
    --out /workspace/gen-output/r$i > /workspace/gen_r$i.log 2>&1 < /dev/null &
  echo "process $i: gpu $gpu, $(wc -l < $f) ids from $f"
  sleep 8
done
nohup bash /workspace/tools/finish_gen.sh > /workspace/finish.log 2>&1 < /dev/null &
echo "STARTED $i processes $(date -u +%H:%M)"
