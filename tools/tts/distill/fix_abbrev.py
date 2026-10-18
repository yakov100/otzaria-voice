"""Expands the Talmud page markers that reached the sentence lists as bare
tokens (the source's geresh was lost before the abbreviation tables ran):
"גמ" -> "גמרא", "מתני" -> "מתניתין", in sentences.tsv (both text columns) and
sentences.vocalized.txt, in place. Counts what it changed.

    python fix_abbrev.py <folder with sentences.tsv and sentences.vocalized.txt>
"""
import os
import re
import sys

NIKUD = "[֑-ׇ]*"
RULES = [
    (re.compile(r"(?<![א-ת])ג" + NIKUD + "מ" + NIKUD + r"(?![א-ת֑-ׇ])"), "גְּמָרָא"),
    (re.compile(r"(?<![א-ת])מ" + NIKUD + "ת" + NIKUD + "נ" + NIKUD + "י" + NIKUD + r"(?![א-ת֑-ׇ])"), "מַתְנִיתִין"),
]
PLAIN = [(re.compile(r"(?<![א-ת])גמ(?![א-ת])"), "גמרא"),
         (re.compile(r"(?<![א-ת])מתני(?![א-ת])"), "מתניתין")]


def fix(text, rules):
    n = 0
    for rx, rep in rules:
        text, k = rx.subn(rep, text)
        n += k
    return text, n


def main():
    folder = sys.argv[1]
    total = 0
    p = os.path.join(folder, "sentences.vocalized.txt")
    lines = open(p, encoding="utf-8").read().split("\n")
    out = []
    for l in lines:
        l2, n = fix(l, RULES)
        total += n
        out.append(l2)
    open(p, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    p = os.path.join(folder, "sentences.tsv")
    rows = open(p, encoding="utf-8").read().split("\n")
    out = []
    for i, r in enumerate(rows):
        cols = r.split("\t")
        if i > 0 and len(cols) >= 5:
            cols[3], a = fix(cols[3], RULES)
            cols[4], b = fix(cols[4], PLAIN)
            total += a + b
        out.append("\t".join(cols))
    open(p, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print(f"{total} replacements in {folder}")


if __name__ == "__main__":
    main()
