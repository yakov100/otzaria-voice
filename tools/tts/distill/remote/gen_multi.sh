#!/bin/bash
# Generation on a multi-GPU machine: one generate_remote.py per "gpu start end"
# triple, each on its own GPU (CUDA_VISIBLE_DEVICES) with a thread limit (a
# process otherwise opens a thread per core and they thrash), each writing to
# gen-output/g<gpu>_<i>. Ids already in any zip under gen-output are skipped,
# so zips copied in from other machines count. Then finish_gen.sh (QC).
#   nohup bash gen_multi.sh <threads> "0 0 720" "1 720 1440" ... > /workspace/multi.log 2>&1 &
set -u
T=$1; shift
source /workspace/venv-cb/bin/activate; cd /workspace
export OMP_NUM_THREADS=$T MKL_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE
i=0
for r in "$@"; do
  i=$((i+1)); set -- $r
  CUDA_VISIBLE_DEVICES=$1 nohup python /workspace/tools/generate_remote.py --start $2 --end $3 \
    --out /workspace/gen-output/g$1_$i > /workspace/gen_g$1_$i.log 2>&1 < /dev/null &
  echo "process $i: gpu $1, lines $2-$3"
  sleep 10
done
nohup bash /workspace/tools/finish_gen.sh > /workspace/finish.log 2>&1 < /dev/null &
echo "STARTED $i processes $(date -u +%H:%M)"
