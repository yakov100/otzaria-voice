"""Ashkenazi ("yeshivish") reading by rewriting the vocalized text, with no
retraining: the light voice learned Israeli Hebrew, so we hand it the marks
that make it say the Ashkenazi sounds it already knows.

  * kamatz -> holam            (בָּרוּךְ reads "boruch")
  * tav without dagesh -> samekh (שַׁבָּת reads "shabbos")
  * stress on the next-to-last syllable when the word is stressed last
    (the stress mark U+05AB the diacritizer writes is moved/added)
  * optional: holam -> "oy" (a silent yod after it), tzere -> "ey"

    python yeshivish.py in.txt out.txt [--oy] [--ey]
Input and output are NFD vocalized lines (with the stress/shva marks).
"""
import re
import sys
import unicodedata

KAMATZ, HOLAM, HOLAM_VAV, DAGESH, STRESS, SHVA_NA, METEG = "ָ", "ֹ", "ֺ", "ּ", "֫", "ֽ", "ֽ"
TZERE, HIRIQ, YOD, VAV = "ֵ", "ִ", "י", "ו"
FULL_VOWELS = set("ִֵֶַָֹֺֻ")  # hiriq..kubutz (not shva, not hataf)
LETTER = re.compile("[א-ת]")


def graphemes(word):
    """[(letter, marks)] for one word; non-letters are their own entries."""
    out = []
    for ch in word:
        if LETTER.match(ch) or not out or not ("֑" <= ch <= "ׇ"):
            out.append([ch, ""])
        else:
            out[-1][1] += ch
    return out


def nucleus(i, g):
    """Is grapheme i a syllable nucleus (carries a full vowel, or is a vowel vav)?"""
    letter, marks = g[i]
    if any(m in FULL_VOWELS for m in marks):
        # a vav with holam/dagesh after a consonant is the vowel itself (וֹ, וּ)
        return True
    if letter == VAV and DAGESH in marks and i > 0 and not any(m in FULL_VOWELS for m in g[i - 1][1]):
        return True  # shuruk
    return False


def convert_word(word, oy=False, ey=False):
    g = graphemes(word)
    holam = [HOLAM in m or HOLAM_VAV in m for _, m in g]  # before kamatz becomes holam: only these say "oy"
    for i, (letter, marks) in enumerate(g):
        if not LETTER.match(letter):
            continue
        marks = marks.replace(KAMATZ, HOLAM)
        if letter == "ת" and DAGESH not in marks:
            letter = "ס"  # samekh
        g[i] = [letter, marks]
    nuclei = [i for i in range(len(g)) if LETTER.match(g[i][0]) and nucleus(i, g)]
    if len(nuclei) >= 2 and not any(STRESS in m for _, m in g):
        # stressed last by default: put the stress on the next-to-last syllable
        g[nuclei[-2]][1] += STRESS
    out = []
    for i, (letter, marks) in enumerate(g):
        out.append(letter + marks)
        nxt = g[i + 1][0] if i + 1 < len(g) else ""
        if oy and holam[i] and nxt not in (YOD, VAV) and i + 1 < len(g):
            out.append(YOD)
        if ey and TZERE in marks and nxt != YOD and i + 1 < len(g):
            out.append(YOD)
    return "".join(out)


def convert(line, oy=False, ey=False):
    line = unicodedata.normalize("NFD", line)
    return " ".join(convert_word(w, oy, ey) for w in line.split(" "))


if __name__ == "__main__":
    oy, ey = "--oy" in sys.argv, "--ey" in sys.argv
    src, dst = [a for a in sys.argv[1:] if not a.startswith("--")]
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        for line in open(src, encoding="utf-8"):
            f.write(convert(line.rstrip("\n"), oy, ey) + "\n")
