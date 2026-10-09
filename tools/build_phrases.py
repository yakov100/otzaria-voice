"""Builds phrases.txt: the most frequent word sequences (2-4 words) of the
Babylonian Talmud's main text in an Otzaria seforim.db. The decoder rewards a
sentence that completes one of them, which helps fixed Aramaic expressions
("תא שמע", "אי בעית אימא") that the speech model hears poorly.

    python build_phrases.py <seforim.db> <out phrases.txt> [--top 1500]

Words are normalized as in the language model (no final letters, no quotes).
"""
import argparse
import collections
import re
import sqlite3

import zstandard

TAG = re.compile(r"<[^>]+>")
NIKUD = re.compile(r"[\u0591-\u05C7]")
WORD = re.compile(r"[א-ת]+(?:[\"'׳״][א-ת]+)?")
QUOTES = re.compile(r"[\"'׳״]")
FINALS = str.maketrans("ךםןףץ", "כמנפצ")
BAVLI = "תלמוד בבלי"
SEDARIM = ("סדר זרעים", "סדר מועד", "סדר נשים", "סדר נזיקין", "סדר קדשים", "סדר טהרות")


def norm(w):
    return QUOTES.sub("", w).translate(FINALS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seforim_db")
    ap.add_argument("out")
    ap.add_argument("--top", type=int, default=1500)
    ap.add_argument("--min-count", type=int, default=30)
    args = ap.parse_args()
    db = sqlite3.connect(f"file:{args.seforim_db}?mode=ro", uri=True)
    (root,) = db.execute("SELECT id FROM category WHERE title = ? AND parentId IS NULL OR "
                         "title = ? AND level = 0", (BAVLI, BAVLI)).fetchone()
    cats = [i for (i,) in db.execute(
        f"SELECT id FROM category WHERE parentId = ? AND title IN ({','.join('?' * len(SEDARIM))})",
        (root, *SEDARIM))]
    books = [i for (i,) in db.execute(
        f"SELECT id FROM book WHERE categoryId IN ({','.join('?' * len(cats))})", cats)]
    zstd = None
    if db.execute("SELECT 1 FROM sqlite_master WHERE name = 'zstd_dict'").fetchone():
        (d,) = db.execute("SELECT dict FROM zstd_dict").fetchone()
        zstd = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(d))
    grams = collections.Counter()
    q = (f"SELECT c.content FROM line l JOIN line_content c ON c.id = l.id "
         f"WHERE l.bookId IN ({','.join('?' * len(books))})")
    lines = 0
    for (content,) in db.execute(q, books):
        if isinstance(content, bytes):
            content = zstd.decompress(content).decode("utf-8")
        text = NIKUD.sub("", TAG.sub(" ", content)).replace("״", '"').replace("׳", "'")
        ws = [norm(w) for w in WORD.findall(text)]
        for n in (2, 3, 4):
            grams.update(zip(*(ws[i:] for i in range(n))))
        lines += 1
    print(f"{len(books)} books, {lines} lines")
    # A longer phrase is kept only when it is not just its shorter part repeated.
    picked = []
    for g, c in grams.most_common():
        if c < args.min_count or len(picked) >= args.top:
            break
        if any(len(w) < 2 for w in g):
            continue
        picked.append((g, c))
    with open(args.out, "w", encoding="utf-8") as f:
        for g, c in picked:
            f.write(" ".join(g) + f"\t{c}\n")
    print(f"wrote {len(picked)} phrases")


if __name__ == "__main__":
    main()
