"""Measures the narrator's nikud against vocalized lines it has not seen.

    python eval_nikud.py <gold.txt> <service exe> <models dir> [--plain plain.txt]

gold.txt holds vocalized lines (the holdout of build_nikud_lexicon.py). The
plain text is written beside it, the service's `--vocalize` mode vocalizes
it, and the result is scored per letter (the class of its marks, and the
shin/sin dot) and per word (every letter right). Run it with and without
nikud_lexicon.bin in the models folder to see what the lexicon adds.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_nikud_lexicon import pattern_of, TOKEN, MARK  # noqa: E402


def words_of(line):
    return [t for t in TOKEN.findall(line.replace("־", " "))]


def main():
    gold_path, exe, models = sys.argv[1], sys.argv[2], sys.argv[3]
    gold = [l.strip() for l in open(gold_path, encoding="utf-8") if l.strip()]
    base = os.path.splitext(gold_path)[0]
    plain_path, out_path = base + ".plain.txt", base + ".vocalized.txt"
    with open(plain_path, "w", encoding="utf-8") as f:
        for l in gold:
            f.write(MARK.sub("", l) + "\n")
    r = subprocess.run([exe, "--vocalize", models, plain_path, out_path])
    if r.returncode != 0:
        sys.exit("vocalize failed: " + str(r.returncode))
    got = [l.rstrip("\n") for l in open(out_path, encoding="utf-8")]
    letters = right = words = words_right = 0
    for g, o in zip(gold, got):
        gw, ow = words_of(g), words_of(o)
        if len(gw) != len(ow):
            continue  # the line was reshaped; skipped rather than guessed
        for a, b in zip(gw, ow):
            pa, pb = pattern_of(a), pattern_of(b)
            if pa is None:
                continue
            words += 1
            if pb is None or len(pa[1]) != len(pb[1]):
                letters += len(pa[1])
                continue
            ok = sum(1 for x, y in zip(pa[1], pb[1]) if x == y)
            letters += len(pa[1])
            right += ok
            words_right += ok == len(pa[1])
    print(f"letters {right}/{letters} = {100.0 * right / max(1, letters):.1f}%  "
          f"words {words_right}/{words} = {100.0 * words_right / max(1, words):.1f}%")


if __name__ == "__main__":
    main()
