"""What the read-aloud plugin says for a text: abbreviations as words, names
as one word, unknown abbreviations and numerals by letter name, the
Tetragrammaton as "השם", teamim dropped. A Python port of toSpeech/rewrites in
plugins/read-aloud/main.js, driven by the tables dump_tables.js writes to
speech_tables.json, so the synthetic training text matches what the plugin
will send the voice.

    from speech_text import to_speech
    to_speech('ת"ר דף ב.')  -> 'תנו רבנן בֵּית עַמּוּד אָלֶף'
"""
import json
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(_HERE, "speech_tables.json"), encoding="utf-8") as _f:
    _T = json.load(_f)
ABBREVIATIONS = _T["abbreviations"]
NAMES = set(_T["names"])
LETTER_NAMES = _T["letterNames"]
NUMERAL_HEADS = re.compile(_T["numeralHeads"])
GEMATRIA = re.compile(_T["gematria"])
NOT_NUMERALS = set(_T["notNumerals"])

WORD = re.compile("[א-ת][֑-ׇ]*(?:[א-ת\"'״׳][֑-ׇ]*)*")
MARKS = re.compile("[֑-ׇ]")
QUOTES = re.compile("[\"']")
DIVINE = re.compile("י[֑-ׇ]*ה[֑-ׇ]*ו[֑-ׇ]*ה[֑-ׇ]*(?![א-ת])")
TEAMIM = re.compile("[֑-ֽ֯]")  # teamim and meteg, as the plugin drops
PREFIXES = "ובלכמשהד"


def spell(word):
    return " ".join(LETTER_NAMES.get(ch, ch) for ch in word)


def is_numeral(word, after_head):
    return 1 <= len(word) <= 3 and GEMATRIA.fullmatch(word) is not None and (after_head or word not in NOT_NUMERALS)


def lookup(table, plain):
    if isinstance(table, set):
        if plain in table:
            return QUOTES.sub("", plain)
        if len(plain) > 2 and plain[0] in PREFIXES and plain[1:] in table:
            return plain[0] + QUOTES.sub("", plain[1:])
        return None
    if plain in table:
        return table[plain]
    if len(plain) > 2 and plain[0] in PREFIXES and plain[1:] in table:
        return plain[0] + table[plain[1:]]
    return None


def rewrites(orig):
    """start -> (length, text) for the words said differently."""
    out = {}
    prev = ""
    for m in WORD.finditer(orig):
        raw = m.group(0)
        plain = MARKS.sub("", raw).replace("״", '"').replace("׳", "'")
        letters = QUOTES.sub("", plain)
        after_head = NUMERAL_HEADS.fullmatch(prev) is not None
        text = None
        if QUOTES.search(plain):
            expansion = lookup(ABBREVIATIONS, plain)
            name = None if expansion else lookup(NAMES, plain)
            if expansion:
                text = expansion
            elif name:
                text = name
            elif re.fullmatch("[א-ת]{1,4}\"[א-ת]", plain):
                text = spell(letters)
            elif re.fullmatch("[א-ת]'", plain):
                text = spell(letters)
            else:
                text = letters
        elif after_head and is_numeral(letters, True):
            text = spell(letters)
        end = m.end()
        after = orig[end] if end < len(orig) else ""
        before = orig[m.start() - 1] if m.start() > 0 else " "
        if (text is None or after_head) and not QUOTES.search(plain) and after in (":", ".") and \
                is_numeral(letters, after_head) and (after_head or after == ":") and before.isspace():
            nxt = orig[end + 1] if end + 1 < len(orig) else None
            if nxt is None or nxt.isspace():
                text = spell(letters) + (" עַמּוּד בֵּית" if after == ":" else " עַמּוּד אָלֶף")
                out[m.start()] = (len(raw) + 1, text)
                prev = plain
                continue
        if text is not None and text != raw:
            out[m.start()] = (len(raw), text)
        prev = plain
    return out


def to_speech(orig):
    rw = rewrites(orig)
    out = []
    i = 0
    n = len(orig)
    while i < n:
        r = rw.get(i)
        if r:
            out.append(r[1])
            i += r[0]
            continue
        m = DIVINE.match(orig, i)
        if m:
            out.append("השם")
            i = m.end()
            continue
        ch = orig[i]
        if TEAMIM.match(ch):
            i += 1
            continue
        out.append(" " if ch in "־׀" else ch)
        i += 1
    return "".join(out)


if __name__ == "__main__":
    import sys
    for line in sys.stdin:
        print(to_speech(line.rstrip("\n")))
