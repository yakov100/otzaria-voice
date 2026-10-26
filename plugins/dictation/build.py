"""Builds the installable forms of the plugin from one source.

    python plugins/dictation/build.py            -> plugins/dictation/packages/released, plugins/dictation/packages/next

released  minAppVersion 0.9.98.1 (Otzaria with search-field actions, see
          docs/dictation/search-field-actions-contract.md): a microphone
          button in the reader's toolbar, a keyboard shortcut, and a
          microphone inside Otzaria's search fields that writes into the
          field. This is the store/release build (from 2.3.0).
next      the slot for a build that needs an Otzaria newer than the released
          one. Today there is none, so it is the same as "released"; the
          workflow still packs it as a test artifact.

Users of Otzaria 0.9.98 keep the 2.2.x plugin: Otzaria 0.9.98 does not know the
search.field_actions permission and refuses the manifest (how it compares a
four-part minAppVersion was not checked), and Otzaria's packager refuses a
plugin that calls an API newer than its minAppVersion (search.setFieldText is
0.9.98.1), so one package cannot serve both.

Each output folder is a complete plugin: lint it, then pack it with
`otzaria pack-plugin <folder>`.
"""

import json
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
DIST = ROOT / "packages"

FILES = ["service.js", "feedback.js", "dictation.js", "toolbar.js", "field_actions.js", "main.js"]
VARIANTS = {
    "released": {"files": FILES},
    "next": {"files": FILES},
}


def manifest_for(name):
    return json.loads((ROOT / "manifest.base.json").read_text(encoding="utf-8"))


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
