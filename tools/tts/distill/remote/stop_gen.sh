#!/bin/bash
# Stops this machine's generators so that every clip made is in a zip (the
# SIGTERM handler writes them), then prints the ids in all zips, one a line.
#   bash stop_gen.sh > done_ids.txt
pkill -f "^bash /workspace/tools/finish_gen.sh"
pkill -f "python /workspace/tools/generate_remote.py"
for k in $(seq 45); do pgrep -f "python /workspace/tools/generate_remote.py" >/dev/null || break; sleep 2; done
pkill -9 -f "python /workspace/tools/generate_remote.py"
python3 - <<'PY'
import glob, zipfile
for z in glob.glob("/workspace/gen-output/**/clips_*.zip", recursive=True):
    try:
        for n in zipfile.ZipFile(z).namelist():
            if n.endswith(".wav"):
                print(n[:-4])
    except zipfile.BadZipFile:
        pass
PY
