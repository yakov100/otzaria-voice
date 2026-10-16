"""Training text for the tiny diacritizer (docs/tts/light-voice.md, stage E):
lines of Hebrew and Aramaic, each vocalized with Phonikud's pronunciation
marks by the installed service ("the teacher": the big diacritizer plus the
Talmudic lexicon, `otzaria-voice --vocalize --marks`).

    python prepare.py --dicta <Talmud-Bavli-with-Nikud clone> [--work ...] [--exe ...] [--pack ...]
        [--target-chars 12000000] [--shard-chars 200000] [--prepare-only | --run-only]

Sources, by share of the target:
  * 40%: Dicta's vocalized Talmud (CC BY-SA). The nikud is kept as it is
    (gold); the teacher only adds the stress and vocal-shva marks.
  * 30%: vocalized books of the local library with a free license (Mishnah,
    Tanakh, siddurim, Zohar; lines at least 80% vocalized). Gold nikud too.
  * 30%: unvocalized books of the library with a free license (halakha,
    responsa, later commentaries: the Hebrew the big model is good at). The
    teacher gives both the nikud and the marks.
Lines are cut to at most 600 characters at a space. The shards go to
<work>/shards/NNN.txt (with NNN.src: one source letter a line, b/v/p), the
teacher's output to <work>/out/NNN.txt; a shard already in out/ is skipped,
so the run can be stopped and continued. Below-normal priority, one shard at
a time (about 6 minutes a shard of 200k characters). make_dataset.py turns
out/ into the training arrays.
"""
import argparse
import collections
import json
import os
import random
import re
import sqlite3
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from build_nikud_lexicon import dicta_lines, tokens_of, LICENSES  # noqa: E402

HEBREW = re.compile("[א-ת]")
MAX_LINE = 600
MIN_LETTERS = 20
SHARES = {"b": 0.4, "v": 0.3, "p": 0.3}


def cut(line):
    """[line] in pieces of at most MAX_LINE characters, cut at a space."""
    line = re.sub(r"\s+", " ", line).strip()
    out = []
    while len(line) > MAX_LINE:
        at = line.rfind(" ", MAX_LINE // 2, MAX_LINE)
        if at < 0:
            at = MAX_LINE
        out.append(line[:at].strip())
        line = line[at:].strip()
    if line:
        out.append(line)
    return out


def usable(line):
    return len(HEBREW.findall(line)) >= MIN_LETTERS


def library_books(db_path, vocalized):
    c = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
    books = c.execute(
        "select b.id, b.title, (select v.license from book_version v where v.bookId=b.id and v.hasContent=1 "
        "order by v.priority limit 1), b.totalLines from book b where b.hasNekudot=?", (1 if vocalized else 0,)).fetchall()
    return c, [(i, t, n) for i, t, lic, n in books if lic and LICENSES.match(lic.strip())]


def book_lines(c, dicts, plain, book_id, limit, rnd):
    import zstandard as zstd
    rows = c.execute(
        "select lc.content from line l join line_content lc on lc.id=l.id where l.bookId=?", (book_id,)).fetchall()
    if len(rows) > limit:
        rows = rnd.sample(rows, limit)
    for (blob,) in rows:
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


def collect(args, rnd):
    """Lines per source, cleaned, cut and shuffled."""
    out = {"b": [], "v": [], "p": []}
    for line in dicta_lines(args.dicta):
        toks = tokens_of(line)
        if toks and sum(t is not None for t in toks) >= 0.8 * len(toks):
            out["b"].extend(l for l in cut(line) if usable(l))
    print(f"dicta: {len(out['b'])} lines, {sum(map(len, out['b'])) / 1e6:.1f}M chars", file=sys.stderr)
    if os.path.exists(args.library):
        import zstandard as zstd
        for key, vocalized in (("v", True), ("p", False)):
            c, books = library_books(args.library, vocalized)
            dicts = {i: zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(d))
                     for i, d in c.execute("select id, dict from zstd_dict")}
            plain = zstd.ZstdDecompressor()
            per_book = max(50, int(args.target_chars * SHARES[key] / 200 / max(1, len(books))) * 4)
            n = 0
            for book_id, title, _ in books:
                for line in book_lines(c, dicts, plain, book_id, per_book, rnd):
                    if vocalized:
                        toks = tokens_of(line)
                        if not toks or sum(t is not None for t in toks) < 0.8 * len(toks):
                            continue
                    elif tokens_of(line) and any(t is not None for t in tokens_of(line)):
                        continue  # partly vocalized: neither gold nor plain
                    for piece in cut(line):
                        if usable(piece):
                            out[key].append(piece)
                            n += 1
            print(f"library {'vocalized' if vocalized else 'plain'}: {len(books)} books, {n} lines, "
                  f"{sum(map(len, out[key])) / 1e6:.1f}M chars", file=sys.stderr)
    for key in out:
        rnd.shuffle(out[key])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dicta", required=True)
    ap.add_argument("--library", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "books", "seforim.db"))
    ap.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "nakdan-tiny"))
    ap.add_argument("--exe", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "OtzariaVoice", "otzaria-voice.exe"))
    ap.add_argument("--pack", default=os.path.join(os.environ.get("APPDATA", ""), "otzaria", "tts", "pack"))
    ap.add_argument("--target-chars", type=int, default=12_000_000)
    ap.add_argument("--shard-chars", type=int, default=200_000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--prepare-only", action="store_true")
    ap.add_argument("--run-only", action="store_true")
    args = ap.parse_args()
    rnd = random.Random(args.seed)
    shards_dir, out_dir = os.path.join(args.work, "shards"), os.path.join(args.work, "out")
    os.makedirs(shards_dir, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)

    if not args.run_only:
        lines = collect(args, rnd)
        # Quotas: a short source leaves its share to the others.
        quota = {k: int(args.target_chars * s) for k, s in SHARES.items()}
        have = {k: sum(map(len, v)) for k, v in lines.items()}
        spare = sum(max(0, quota[k] - have[k]) for k in quota)
        for k in quota:
            if have[k] > quota[k] and spare > 0:
                extra = min(spare, have[k] - quota[k])
                quota[k] += extra
                spare -= extra
        chosen = []
        stats = collections.Counter()
        for k, lst in lines.items():
            total = 0
            for l in lst:
                if total >= quota[k]:
                    break
                chosen.append((k, l))
                total += len(l)
                stats[k] += len(l)
        rnd.shuffle(chosen)
        shard, size, n = [], 0, 0
        def flush():
            nonlocal shard, size, n
            if not shard:
                return
            with open(os.path.join(shards_dir, f"{n:03d}.txt"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(l for _, l in shard) + "\n")
            with open(os.path.join(shards_dir, f"{n:03d}.src"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(k for k, _ in shard) + "\n")
            n += 1
            shard, size = [], 0
        for k, l in chosen:
            shard.append((k, l))
            size += len(l)
            if size >= args.shard_chars:
                flush()
        flush()
        json.dump({"shards": n, "chars": dict(stats), "lines": len(chosen), "target": args.target_chars},
                  open(os.path.join(args.work, "manifest.json"), "w"), indent=1)
        print(f"{n} shards, {len(chosen)} lines, chars by source {dict(stats)}", file=sys.stderr)
        if args.prepare_only:
            return

    shards = sorted(f for f in os.listdir(shards_dir) if f.endswith(".txt"))
    for name in shards:
        out = os.path.join(out_dir, name)
        if os.path.exists(out):
            continue
        tmp = out + ".tmp"
        t = time.time()
        r = subprocess.run([args.exe, "--vocalize", args.pack, os.path.join(shards_dir, name), tmp, "--marks"],
                           creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        if r.returncode != 0 or not os.path.exists(tmp):
            print(f"{name}: vocalize failed ({r.returncode})", file=sys.stderr)
            sys.exit(1)
        os.replace(tmp, out)
        print(f"{name}: {time.time() - t:.0f} s", file=sys.stderr)
    print("done", file=sys.stderr)


if __name__ == "__main__":
    main()
