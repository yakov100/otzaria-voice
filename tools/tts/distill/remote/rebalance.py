"""Splits the lines nobody has generated yet evenly between generator
processes on several machines: reads sentences.tsv and the done-id lists
(stop_gen.sh output, one file a machine), writes <out>/<machine>_<i>.txt.

    python rebalance.py sentences.tsv out_dir done_a.txt done_b.txt ... -- nl1:4 nl2:4 us:5 tw:16
"""
import os
import sys

args = sys.argv[1:]
cut = args.index("--")
tsv, out, done_files, machines = args[0], args[1], args[2:cut], args[cut + 1:]
ids = [line.split("\t", 1)[0] for line in open(tsv, encoding="utf-8").read().splitlines()[1:] if line]
done = set()
for f in done_files:
    done.update(l.strip() for l in open(f, encoding="utf-8") if l.strip())
todo = [i for i in ids if i not in done]
procs = [(m.split(":")[0], k) for m in machines for k in range(1, int(m.split(":")[1]) + 1)]
os.makedirs(out, exist_ok=True)
per = -(-len(todo) // len(procs))
for n, (name, k) in enumerate(procs):
    part = todo[n * per:(n + 1) * per]
    open(os.path.join(out, f"{name}_{k}.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(part) + "\n")
print(f"{len(ids)} lines, {len(done)} done, {len(todo)} to do: {len(procs)} processes x ~{per}")
