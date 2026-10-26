"""Builds nikud_lexicon.bin: how words of Talmudic Hebrew and Aramaic are
vocalized, learnt from vocalized texts, for the narrator's diacritizer (which
was trained on modern Hebrew and reads Aramaic poorly).

    python build_nikud_lexicon.py --dicta <clone of Talmud-Bavli-with-Nikud>
        [--library %APPDATA%\\otzaria\\books\\seforim.db] [--out nikud_lexicon.bin]
        [--holdout holdout.txt]

Sources:
  * Dicta's vocalized Babylonian Talmud (CC BY-SA; the repo's *.txt files in
    both spellings).
  * Vocalized books of the local Otzaria library whose default version is
    public domain or CC-BY/CC-BY-SA: lines where at least 80% of the words
    carry nikud (Mishnah, Zohar, siddurim; partly vocalized books contribute
    only their vocalized lines). Needs `pip install zstandard`.

The file (read by nikud_lexicon.cpp):
  "OTZNIKUD", u32 version=1, u32 words, u32 bigrams, then the entries.
  A word entry:   u8 n, n letters (byte = code point - 0x05D0), n patterns.
  A bigram entry: u8 n1, n1 letters, u8 n2, n2 letters, n2 patterns of word 2.
  A pattern byte: the diacritizer's class of the letter's marks (0..28, the
  order of kNikudClasses in narrator.cpp; 0 = no mark), plus 0x20 for a sin
  dot and 0x40 for a shin dot.
A word is kept when it was seen at least 3 times and one pattern covers 60%
of its uses. A bigram is kept when it changes the second word's pattern.
"""
import argparse
import collections
import io
import os
import random
import re
import sqlite3
import struct
import sys
import zipfile

# Marks after a letter -> class (narrator.cpp kNikudClasses). Dagesh (05BC)
# adds 13 to the vowel classes 3..14, is class 2 alone, and 28 with 05C7.
VOWELS = {
    "": 0, "ְ": 3, "ֱ": 4, "ֲ": 5, "ֳ": 6, "ִ": 7,
    "ֵ": 8, "ֶ": 9, "ַ": 10, "ָ": 11, "ֹ": 12,
    "ֺ": 13, "ֻ": 14, "ׇ": 27,
}
DAGESH, SIN, SHIN = "ּ", "ׂ", "ׁ"
LETTER = re.compile("[א-ת]")
MARK = re.compile("[֑-ׇ]")
NIKUD = re.compile("[ְ-ׇּׁׂ]")
# A token: letters with their marks, possibly with maqaf-joined parts split off.
TOKEN = re.compile("[א-ת][א-ת֑-ׇ]*")
MIN_COUNT = 3
WORD_SHARE = 0.6
BIGRAM_SHARE = 0.7
MAX_BIGRAMS = 250_000
LICENSES = re.compile(r"^(pd|public domain|cc-by|cc-by-sa|cc by|cc by-sa)$", re.I)


def pattern_of(token):
    """The plain letters and one pattern byte per letter, or None when a mark
    combination is outside the diacritizer's classes."""
    plain, pats = [], []
    i = 0
    while i < len(token):
        c = token[i]
        i += 1
        if not LETTER.match(c):
            return None
        marks = ""
        while i < len(token) and MARK.match(token[i]):
            marks += token[i]
            i += 1
        vowel = "".join(m for m in marks if m in VOWELS and m)
        if len(vowel) > 1:
            return None
        cls = VOWELS[vowel]
        if DAGESH in marks:
            if cls == 0:
                cls = 2
            elif 3 <= cls <= 14:
                cls += 12
            elif cls == 27:
                cls = 28
        flags = (0x20 if SIN in marks else 0) | (0x40 if SHIN in marks else 0)
        if c == "ש" and not flags:
            return None  # a shin without its dot: not fully vocalized
        if flags and c != "ש":
            return None
        plain.append(c)
        pats.append(cls | flags)
    return "".join(plain), bytes(pats)


def tokens_of(line):
    """(plain, pattern) of each vocalized word, None for an unvocalized one."""
    out = []
    for t in TOKEN.findall(line.replace("־", " ")):
        if not NIKUD.search(t):
            out.append(None)
            continue
        out.append(pattern_of(t))
    return out


def dicta_lines(folder):
    for root, _, files in os.walk(folder):
        for f in files:
            if f.endswith(".txt"):
                with open(os.path.join(root, f), encoding="utf-8") as fh:
                    for line in fh:
                        line = line.strip()
                        if line:
                            yield line


def library_lines(db_path):
    import zstandard as zstd
    c = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
    dicts = {i: zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(d))
             for i, d in c.execute("select id, dict from zstd_dict")}
    plain = zstd.ZstdDecompressor()
    books = c.execute(
        "select b.id, b.title, (select v.license from book_version v where v.bookId=b.id and v.hasContent=1 "
        "order by v.priority limit 1) from book b where b.hasNekudot=1").fetchall()
    kept, skipped = [], collections.Counter()
    for book_id, title, license_ in books:
        if not license_ or not LICENSES.match(license_.strip()):
            skipped[license_ or "none"] += 1
            continue
        kept.append((book_id, title, license_))
    print(f"library: {len(kept)} vocalized books with a free license; skipped by license: {dict(skipped)}",
          file=sys.stderr)
    for book_id, title, license_ in kept:
        for (blob,) in c.execute(
                "select lc.content from line l join line_content lc on lc.id=l.id where l.bookId=?", (book_id,)):
            if blob is None:
                continue
            try:
                did = zstd.get_frame_parameters(blob).dict_id
                text = (dicts.get(did) or plain).decompress(blob).decode("utf-8")
            except Exception:
                continue
            for line in re.split(r"[\r\n]+|<br\s*/?>", re.sub(r"<[^>]+>", " ", text)):
                line = line.strip()
                if line:
                    yield line


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dicta", required=True)
    ap.add_argument("--library", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "books", "seforim.db"))
    ap.add_argument("--no-library", action="store_true")
    ap.add_argument("--out", default="nikud_lexicon.bin")
    ap.add_argument("--holdout", default="", help="write this many Dicta lines aside for evaluation")
    ap.add_argument("--holdout-count", type=int, default=300)
    args = ap.parse_args()

    rnd = random.Random(7)
    words = collections.defaultdict(collections.Counter)
    bigrams = collections.defaultdict(collections.Counter)
    holdout = []
    stats = collections.Counter()

    def learn(line):
        toks = tokens_of(line)
        vocalized = [t for t in toks if t is not None]
        if not toks or len(vocalized) < 0.8 * len(toks):
            stats["lines skipped"] += 1
            return
        stats["lines"] += 1
        prev = None
        for t in toks:
            if t is None:
                prev = None
                continue
            plain, pat = t
            stats["tokens"] += 1
            words[plain][pat] += 1
            if prev is not None:
                bigrams[(prev, plain)][pat] += 1
            prev = plain

    for line in dicta_lines(args.dicta):
        if args.holdout and len(holdout) < args.holdout_count and rnd.random() < 0.002:
            holdout.append(line)
            continue
        learn(line)
    print(f"dicta: {stats['lines']} lines, {stats['tokens']} tokens", file=sys.stderr)
    if not args.no_library and os.path.exists(args.library):
        before = stats["tokens"]
        for line in library_lines(args.library):
            learn(line)
        print(f"library: {stats['tokens'] - before} tokens", file=sys.stderr)

    chosen = {}
    for plain, counter in words.items():
        total = sum(counter.values())
        pat, n = counter.most_common(1)[0]
        if total >= MIN_COUNT and n >= WORD_SHARE * total:
            chosen[plain] = pat
    pairs = []
    for (a, b), counter in bigrams.items():
        total = sum(counter.values())
        pat, n = counter.most_common(1)[0]
        if total < MIN_COUNT or n < BIGRAM_SHARE * total:
            continue
        if chosen.get(b) == pat:
            continue  # the word alone already says so
        pairs.append((total, a, b, pat))
    pairs.sort(reverse=True)
    pairs = pairs[:MAX_BIGRAMS]

    def letters(s):
        return bytes(ord(ch) - 0x05D0 for ch in s)

    body = io.BytesIO()
    for plain in sorted(chosen):
        body.write(struct.pack("<B", len(plain)) + letters(plain) + chosen[plain])
    for _, a, b, pat in sorted(pairs, key=lambda p: (p[1], p[2])):
        body.write(struct.pack("<B", len(a)) + letters(a) + struct.pack("<B", len(b)) + letters(b) + pat)
    data = b"OTZNIKUD" + struct.pack("<III", 1, len(chosen), len(pairs)) + body.getvalue()
    with open(args.out, "wb") as f:
        f.write(data)
    zipped = io.BytesIO()
    with zipfile.ZipFile(zipped, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(os.path.basename(args.out), data)
    with open(args.out + ".zip", "wb") as f:
        f.write(zipped.getvalue())

    if args.holdout:
        with open(args.holdout, "w", encoding="utf-8") as f:
            f.write("\n".join(holdout))
        covered = total_h = 0
        for line in holdout:
            for t in tokens_of(line):
                if t is None:
                    continue
                total_h += 1
                covered += t[0] in chosen
        print(f"holdout: {len(holdout)} lines; words known to the lexicon: {covered}/{total_h} "
              f"({100.0 * covered / max(1, total_h):.1f}%)", file=sys.stderr)
    print(f"words {len(chosen)} (of {len(words)} seen), bigrams {len(pairs)}; "
          f"{len(data) / 1e6:.2f} MB raw, {len(zipped.getvalue()) / 1e6:.2f} MB zipped -> {args.out}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
