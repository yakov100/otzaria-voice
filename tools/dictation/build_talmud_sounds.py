"""Builds talmud_sounds.tsv: how each Talmud phrase sounds in yeshivish pronunciation.

    python build_talmud_sounds.py <dicta dir> <talmud_phrases.txt> <out.tsv>

<dicta dir>: Dicta's pointed Bavli, one folder per tractate with the daf files
(`<tractate>/txt/*.txt`, from the `*__nikud_meteg_files.zip` archives listed in
https://files.dicta.org.il/library-1-0/books_shas.json). License CC BY-SA 4.0,
so the output is released under CC BY-SA 4.0 too (see LICENSES.txt).

For every phrase the two most common pointed forms are taken from the text, and
each is respelled the way the recognizer writes the sound. Three spellings per form:
full yeshivish, yeshivish with a kamatz inside the word kept "a" (אִיבָּעֵית =
איבעיס), and plain pronunciation; in all three a final mappiq he is silent
(לֵיהּ = לי). The yeshivish rules:
  - kamatz -> "o" (תָּא = תו), holam -> "oy" (אוֹמֵר = אוימר), tav rafa -> ס;
  - a final kamatz + silent alef/he in a longer word -> "e" (רָבָא = רובה,
    הוּנָא = הונה),
    a one-syllable word keeps "o" (לָא = לו, מְנָא = מנו);
  - לַן is said "lon".
Output: `phrase<TAB>sound[<TAB>sound]`, only where a sound differs from the phrase.
The second pass (phrase_rescorer.dart) scores the audio against these spellings
and still writes the phrase.
"""
import glob
import os
import re
import sys
from collections import Counter, defaultdict

DAGESH, KAMATZ, CHOLAM = "ּ", "ָ", "ֹ"
MARKS = re.compile("[֑-ֽ֯׀׃-׆]")  # cantillation, meteg, punctuation
POINTS = re.compile("[֑-ׇ]")
VOWELS = re.compile("[ֱ-ׇֹֻ]")  # any vowel but shva
WORDS = {"לַן": "לון"}


def clusters(word):
    out = []
    for ch in word:
        if out and "֑" <= ch <= "ׇ":
            out[-1] += ch
        else:
            out.append(ch)
    return out


def yeshivish(text, kamatz_o=True):
    """kamatz_o=False: a kamatz inside the word keeps "a" (אִיבָּעֵית = איבעיס), the
    other rules stay."""
    words = []
    for w in text.split():
        if w in WORDS:
            words.append(WORDS[w])
            continue
        cl, res = clusters(w), []
        for i, c in enumerate(cl):
            base, marks = c[0], c[1:]
            nxt = cl[i + 1] if i + 1 < len(cl) else ""
            if base == "ו" and CHOLAM in marks:
                res.append("וי")
                continue
            if base == "ה" and DAGESH in marks and i == len(cl) - 1 and i > 0:
                break  # mappiq he is silent: לֵיהּ = לי
            res.append("ס" if base == "ת" and DAGESH not in marks else base)
            if KAMATZ in marks:
                if nxt and nxt[0] in "אה" and len(nxt) == 1 and i + 2 == len(cl):
                    # shuruk (וּ after a letter) is a vowel too: הוּנָא = הונה
                    vowels = sum(1 for k, c2 in enumerate(cl[: i + 1])
                                 if VOWELS.search(c2) or (k > 0 and c2[0] == "ו" and DAGESH in c2))
                    res.append("ה" if vowels > 1 else ("ו" if kamatz_o else nxt[0]))
                    break
                if kamatz_o:
                    res.append("ו")
            elif CHOLAM in marks:
                res.append("וי")
        words.append("".join(res))
    return " ".join(words)


def regular(text):
    """Plain pronunciation, only the silent mappiq he dropped (לֵיהּ = לי)."""
    words = []
    for w in text.split():
        cl = clusters(w)
        if len(cl) > 1 and cl[-1][0] == "ה" and DAGESH in cl[-1]:
            cl = cl[:-1]
        words.append(POINTS.sub("", "".join(cl)))
    return " ".join(words)


def sounds_of(form):
    return [yeshivish(form), yeshivish(form, kamatz_o=False), regular(form)]


def pointed_forms(dicta_dir, phrases):
    words = []
    for f in sorted(glob.glob(os.path.join(dicta_dir, "*", "txt", "*.txt"))):
        for w in re.findall(r"[א-ת֑-ׇ]+", open(f, encoding="utf-8").read()):
            w = MARKS.sub("", w)
            words.append((POINTS.sub("", w), w))
    want = {tuple(p.split()) for p in phrases}
    lens = {len(k) for k in want}
    forms = defaultdict(Counter)
    plain = [p for p, _ in words]
    for i in range(len(plain)):
        for n in lens:
            k = tuple(plain[i:i + n])
            if k in want:
                forms[k][" ".join(w for _, w in words[i:i + n])] += 1
    return forms


def main():
    dicta_dir, phrases_path, out_path = sys.argv[1:4]
    phrases = [l.split("\t")[0] for l in open(phrases_path, encoding="utf-8").read().splitlines() if l.strip()]
    forms = pointed_forms(dicta_dir, phrases)
    n = 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Yeshivish spellings of Talmud phrases, from Dicta's pointed Bavli (CC BY-SA 4.0).\n")
        for p in phrases:
            sounds = []
            for form, _ in forms.get(tuple(p.split()), Counter()).most_common(2):
                for s in sounds_of(form):
                    if s != p and s not in sounds and len(s.split()) == len(p.split()):
                        sounds.append(s)
            if sounds:
                out.write("\t".join([p, *sounds]) + "\n")
                n += 1
    print(f"{n} of {len(phrases)} phrases have yeshivish spellings")


if __name__ == "__main__":
    main()
