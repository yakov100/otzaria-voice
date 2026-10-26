"""Builds word_sounds.tsv: frequent Bavli words written differently from how they sound.

    python build_word_sounds.py <dicta dir> <out.tsv> [--min 100]

For every word seen at least --min times in Dicta's pointed Bavli (see
build_talmud_sounds.py; CC BY-SA 4.0, so the output is CC BY-SA 4.0 too), its two
most common pointed forms are respelled as they sound (yeshivish, yeshivish with
kamatz "a", plain with a silent mappiq he). Output: `sound<TAB>word[<TAB>word]`.

A sound that is itself a common word is left out: "אומר" is meant as אומר, not
as a yeshivish אמר. It stays only when it is rare (under --min) or far rarer
than the word (under a tenth: "קרו" 126 times, קרא 2078).

The second pass (phrase_rescorer.dart) lets a word the recognizer wrote exactly
as such a sound be written as the word, when the LM prefers it.
"""
import argparse
import glob
import os
import re
from collections import Counter, defaultdict

from build_talmud_sounds import MARKS, POINTS, sounds_of


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dicta_dir")
    ap.add_argument("out")
    ap.add_argument("--min", type=int, default=100)
    args = ap.parse_args()
    forms = defaultdict(Counter)
    for f in glob.glob(os.path.join(args.dicta_dir, "*", "txt", "*.txt")):
        for w in re.findall(r"[א-ת֑-ׇ]+", open(f, encoding="utf-8").read()):
            w = MARKS.sub("", w)
            forms[POINTS.sub("", w)][w] += 1
    out = defaultdict(set)
    for plain, c in forms.items():
        n_w = sum(c.values())
        if n_w < args.min or len(plain) < 2:
            continue
        for form, _ in c.most_common(2):
            for s in sounds_of(form):
                n_s = sum(forms.get(s, {}).values())
                if s != plain and " " not in s and (n_s < args.min or n_s < 0.1 * n_w):
                    out[s].add(plain)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fo:
        fo.write("# Sounds of frequent Bavli words, from Dicta's pointed Bavli (CC BY-SA 4.0).\n")
        for s in sorted(out):
            fo.write("\t".join([s, *sorted(out[s])]) + "\n")
    print(len(out), "sounds")


if __name__ == "__main__":
    main()
