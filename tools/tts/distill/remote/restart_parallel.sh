#!/bin/bash
# Replaces this machine's single generate_remote.py with several processes on
# the same GPU (one process left the 4090 at ~25%: the generation is bound by
# Python), after the first zip, so almost nothing already made is lost. The
# first range writes into /workspace/gen-output (where the first zip is, so
# its clips are skipped), the others into p2, p3, ...; finish_gen.sh gathers them.
#   nohup bash restart_parallel.sh "0 1439" "1439 2878" "2878 4317" > /workspace/restart.log 2>&1 &
set -u
until ls /workspace/gen-output/clips_0001.zip >/dev/null 2>&1; do sleep 15; done
pkill -f "^bash /workspace/tools/finish_gen.sh"
pkill -f "generate_remote.py"; sleep 8
source /workspace/venv-cb/bin/activate; cd /workspace
i=0
for r in "$@"; do
  i=$((i+1)); set -- $r
  if [ $i = 1 ]; then out=/workspace/gen-output; else out=/workspace/gen-output/p$i; fi
  nohup python /workspace/tools/generate_remote.py --start $1 --end $2 --out $out > /workspace/gen$i.log 2>&1 < /dev/null &
  echo "process $i: lines $1-$2 -> $out"
  sleep 20
done
nohup bash /workspace/tools/finish_gen.sh > /workspace/finish.log 2>&1 < /dev/null &
echo "RESTARTED $i processes $(date -u +%H:%M)"
