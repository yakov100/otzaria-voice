"""Builds the two installable forms of the plugin from one source.

    python plugins/dictation/build.py            -> plugins/dictation/packages/released, plugins/dictation/packages/next

released  minAppVersion 0.9.98 (Otzaria as released today): a microphone
          button in the reader's toolbar and a keyboard shortcut; the result
          opens in a search tab.
next      minAppVersion 0.9.98.1 (Otzaria with search-field actions, see
          docs/dictation/search-field-actions-contract.md): the same, plus a microphone
          inside Otzaria's search fields that writes into the field.

Why two: Otzaria rejects a manifest whose permission it does not know
(search.field_actions on 0.9.98), and its packager rejects a plugin that
calls an API newer than its minAppVersion (search.setFieldText is 0.9.98.1).
So the field-action code and declarations exist only in the "next" build.

Each output folder is a complete plugin: lint it, then pack it with
`otzaria pack-plugin <folder>`.
"""

import json
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
DIST = ROOT / "packages"

COMMON = ["service.js", "feedback.js", "dictation.js", "toolbar.js"]
VARIANTS = {
    "released": {"files": COMMON + ["main.js"]},
    "next": {"files": COMMON + ["field_actions.js", "main.js"]},
}

# The "next" package is the plugin's next version: what the store gets once an
# Otzaria with search-field actions (0.9.98.1) is released. It is newer than the
# released package, so it installs over it.
NEXT_VERSION = "2.3.0"

FIELD_ACTION = {
    "id": "dictate",
    "title": "חיפוש בדיבור",
    "icon": "mic_24_regular",
    "activeIcon": "mic_24_filled",
    "fields": ["fullText", "library", "inBook", "findRef"],
}


def manifest_for(name):
    m = json.loads((ROOT / "manifest.base.json").read_text(encoding="utf-8"))
    if name == "next":
        m["version"] = NEXT_VERSION
        m["minAppVersion"] = "0.9.98.1"
        m["permissions"].append("search.field_actions")
        m["contributes"]["startup"]["searchFieldActions"] = [FIELD_ACTION]
    return m


def bundle(files):
    # One classic script: on Linux and Android Otzaria loads a headless entry
    # from file://, where ES modules are blocked.
    parts = ["/* חיפוש בדיבור — built by plugins/dictation/build.py from plugins/dictation/src; do not edit. */",
             "(function () {", "'use strict';", ""]
    for f in files:
        parts.append((SRC / f).read_text(encoding="utf-8").rstrip() + "\n")
    parts.append("})();\n")
    return "\n".join(parts)


def main():
    for name, v in VARIANTS.items():
        out = DIST / name
        if out.exists():
            shutil.rmtree(out)
        (out / "icon").mkdir(parents=True)
        shutil.copy(ROOT / "icon" / "icon.png", out / "icon" / "icon.png")
        (out / "manifest.json").write_text(
            json.dumps(manifest_for(name), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8", newline="\n")
        (out / "main.js").write_text(bundle(v["files"]), encoding="utf-8", newline="\n")
        print(out)


if __name__ == "__main__":
    main()
