#!/bin/bash
# One generator slot: generate_remote.py on an id list, restarted every 200
# clips (exit code 3) until the list is done.
#   bash gen_loop.sh <ids file> <out dir>
while true; do
  python /workspace/tools/generate_remote.py --ids-file "$1" --end -1 --out "$2" --max-clips 200
  [ $? -eq 3 ] || break
done
