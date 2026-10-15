"""Picks the sentences the light voice is trained on: Torah text in the mix a
reader meets (Gemara, Mishnah, Tanakh and prayers, Rashi, halakha), cut into
sentences of 40-200 characters and written the way the plugin says them
(speech_text.to_speech: abbreviations as words, numerals by letter name).

    python select_text.py --dicta <Talmud-Bavli-with-Nikud clone> --out <dir>
        [--library %APPDATA%\\otzaria\\books\\seforim.db] [--target-words 300000]

Writes to <dir>:
  sentences.tsv        id, source, book, vocalized (1 if the source had nikud), text
  sentences.plain.txt  one sentence a line, for `otzaria-voice.exe --vocalize --marks`
  pilot.txt            the first --pilot lines (about two hours of speech)
  sentences.stats.json counts and words per source

Sources are interleaved by quota, so every prefix of the list has the same
mix. Only books whose text is public domain or CC BY/CC BY-SA are used, plus
traditional prayer books and Rashi whose digitized copies list no license
(the text itself is centuries old; it is read, not redistributed).
"""
import argparse
import collections
import json
import os
import random
import re
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from speech_text import to_speech  # noqa: E402

NIKUD = re.compile("[ְ-ׇּׁׂ]")
MARKS = re.compile("[֑-ׇ]")
HEBREW = re.compile("[א-ת]")
BAD = re.compile("[0-9A-Za-z<>{}\\\\/_=@#$%^*+~`]")
BRACKETS = re.compile(r"\[[^\]]*\]|\([^)]*\)|\{[^}]*\}")
SPLIT = re.compile("(?<=[.?!:;׃])\\s+")
TAG = re.compile(r"<[^>]+>")
MIN_CHARS, MAX_CHARS, MIN_WORDS = 40, 200, 5
FREE = re.compile(r"^(pd|public domain|cc-by|cc-by-sa|cc by|cc by-sa)$", re.I)

# Share of the words per source, and which library books feed each.
QUOTAS = {"gemara": 0.45, "mishnah": 0.20, "tanakh_siddur": 0.15, "rashi": 0.10, "halakha": 0.10}
BOOKS = {
    "mishnah": {"like": "משנה %", "category_like": "סדר %"},
    "tanakh_siddur": {"titles": ["בראשית", "שמות", "ויקרא", "במדבר", "דברים", "תהלים", "משלי", "רות", "שיר השירים",
                                 "קהלת", "אסתר", "סידור ספרד", "סידור ספרד לשבת", "סידור ספרד לימות החול"],
                      "allow_no_license": True},
    "rashi": {"like": "רש\"י על %", "category_like": "סדר %", "allow_no_license": True},
    "halakha": {"like": "משנה תורה, הלכות %", "titles": ["ספר החינוך", "מסילת ישרים", "חובות הלבבות", "שערי תשובה",
                                                        "אורחות צדיקים"]},
}


def dicta_lines(folder):
    for root, _, files in os.walk(folder):
        for f in sorted(files):
            if not f.endswith("haser.txt"):
                continue
            book = f.split(".")[0]
            with open(os.path.join(root, f), encoding="utf-8") as fh:
                for line in fh:
                    line = line.strip()
                    if not line or line.startswith("***") or len(line) < 20:
                        continue
                    yield book, line


class Library:
    def __init__(self, db_path):
        import zstandard as zstd
        self.c = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
        self.dicts = {i: zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(d))
                      for i, d in self.c.execute("select id, dict from zstd_dict")}
        self.plain = zstd.ZstdDecompressor()
        self.zstd = zstd

    def books(self, spec):
        q = ("select b.id, b.title, (select v.license from book_version v where v.bookId=b.id and v.hasContent=1 "
             "order by v.priority limit 1) from book b join category c on c.id=b.categoryId where ")
        rows = []
        if "like" in spec:
            cond = "b.title like ?"
            args = [spec["like"]]
            if "category_like" in spec:
                cond += " and c.title like ?"
                args.append(spec["category_like"])
            rows += self.c.execute(q + cond, args).fetchall()
        if "titles" in spec:
            rows += self.c.execute(q + "b.title in (%s)" % ",".join("?" * len(spec["titles"])), spec["titles"]).fetchall()
        out = []
        for book_id, title, lic in rows:
            lic = (lic or "").strip()
            if FREE.match(lic) or (not lic and spec.get("allow_no_license")):
                out.append((book_id, title))
        return out

    def lines(self, book_id):
        for (blob,) in self.c.execute(
                "select lc.content from line l join line_content lc on lc.id=l.id where l.bookId=? order by l.lineIndex",
                (book_id,)):
            if blob is None:
                continue
            try:
                did = self.zstd.get_frame_parameters(blob).dict_id
                text = (self.dicts.get(did) or self.plain).decompress(blob).decode("utf-8")
            except Exception:
                continue
            for line in re.split(r"[\r\n]+|<br\s*/?>", TAG.sub(" ", text)):
                line = line.strip()
                if line:
                    yield line


def sentences_of(line):
    """Clean sentences of a line, in the plugin's spoken form."""
    line = BRACKETS.sub(" ", line)
    line = re.sub(r"\s+", " ", line).strip()
    for s in SPLIT.split(line):
        s = s.strip(" -–—:;,")
        if not s or BAD.search(s):
            continue
        spoken = to_speech(s).strip()
        plain = MARKS.sub("", spoken)
        if not (MIN_CHARS <= len(plain) <= MAX_CHARS):
            continue
        words = plain.split()
        if len(words) < MIN_WORDS:
            continue
        letters = sum(1 for ch in plain if HEBREW.match(ch))
        if letters < 0.6 * len(plain.replace(" ", "")):
            continue
        if sum(1 for w in words if len(w) == 1) > len(words) / 3:
            continue  # spelt-out leftovers
        yield spoken, plain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dicta", required=True)
    ap.add_argument("--library", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "books", "seforim.db"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--target-words", type=int, default=300_000)
    ap.add_argument("--pilot", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rnd = random.Random(args.seed)

    pool = collections.defaultdict(list)  # source -> [(book, spoken, plain)]
    seen = set()

    def add(source, book, spoken, plain):
        key = plain[:60]
        if key in seen:
            return
        seen.add(key)
        pool[source].append((book, spoken, plain))

    for book, line in dicta_lines(args.dicta):
        for spoken, plain in sentences_of(line):
            add("gemara", book, spoken, plain)
    print(f"gemara: {len(pool['gemara'])} sentences", file=sys.stderr)

    lib = Library(args.library)
    for source, spec in BOOKS.items():
        books = lib.books(spec)
        for book_id, title in books:
            for line in lib.lines(book_id):
                for spoken, plain in sentences_of(line):
                    add(source, title, spoken, plain)
        print(f"{source}: {len(books)} books, {len(pool[source])} sentences", file=sys.stderr)

    # Shuffle each source, then draw by quota so every prefix keeps the mix.
    for source in pool:
        rnd.shuffle(pool[source])
    budget = {s: int(args.target_words * q) for s, q in QUOTAS.items()}
    used = {s: 0 for s in QUOTAS}
    cursor = {s: 0 for s in QUOTAS}
    chosen = []
    while True:
        progressed = False
        for source, quota in QUOTAS.items():
            # The source furthest behind its share goes next; one sentence a turn.
            if cursor[source] >= len(pool[source]) or used[source] >= budget[source]:
                continue
            book, spoken, plain = pool[source][cursor[source]]
            cursor[source] += 1
            used[source] += len(plain.split())
            chosen.append((source, book, spoken, plain))
            progressed = True
        if not progressed:
            break

    with open(os.path.join(args.out, "sentences.tsv"), "w", encoding="utf-8", newline="\n") as tsv, \
            open(os.path.join(args.out, "sentences.plain.txt"), "w", encoding="utf-8", newline="\n") as txt, \
            open(os.path.join(args.out, "pilot.txt"), "w", encoding="utf-8", newline="\n") as pilot:
        tsv.write("id\tsource\tbook\tvocalized\ttext\n")
        for i, (source, book, spoken, plain) in enumerate(chosen, 1):
            words = spoken.split()
            vocalized = sum(1 for w in words if NIKUD.search(w)) >= 0.9 * len(words)
            sid = f"s{i:06d}"
            tsv.write(f"{sid}\t{source}\t{book}\t{int(vocalized)}\t{spoken}\n")
            txt.write(spoken + "\n")
            if i <= args.pilot:
                pilot.write(spoken + "\n")
    stats = {
        "sentences": len(chosen),
        "words": sum(len(p.split()) for _, _, _, p in chosen),
        "per_source": {s: {"sentences": sum(1 for c in chosen if c[0] == s), "words": used[s],
                           "available": len(pool[s])} for s in QUOTAS},
        "estimated_hours": sum(len(p) for _, _, _, p in chosen) / 12 / 3600,
    }
    with open(os.path.join(args.out, "sentences.stats.json"), "w", encoding="utf-8") as f:
        json.dump(stats, f, ensure_ascii=False, indent=1)
    print(json.dumps(stats, ensure_ascii=False, indent=1), file=sys.stderr)


if __name__ == "__main__":
    main()
