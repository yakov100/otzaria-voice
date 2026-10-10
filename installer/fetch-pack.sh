#!/usr/bin/env bash
# Downloads the Hebrew voice pack (models) from this repository's dedicated
# GitHub Release, verifies its size and SHA-256, and unpacks it into a pack
# directory in the exact layout the service expects (the five pack files plus
# version.txt).
#
#   fetch-pack.sh <cache-dir> <out-dir>
#
# The zip is kept in <cache-dir> so CI can cache it between runs; a cached copy
# is re-verified before use and downloaded again if it does not match.
set -euo pipefail

PACK_URL="https://github.com/yakov100/otzaria-voice/releases/download/voice-pack-v3/otzaria-voice-he-v3.zip"
PACK_SIZE=334167487
PACK_SHA256="bde4cfd19a2a581f7836267ddd7ece700bf93c51e122306d6a567958f4c7eb1e"
PACK_VERSION=3
PACK_FILES=(ctc/model.int8.onnx ctc/vocab.json ctc/preprocessor.json lm/library.otzlm LICENSES.txt)

[ $# -eq 2 ] || { echo "usage: $0 <cache-dir> <out-dir>" >&2; exit 2; }
cache="$1"
out="$2"
zip="$cache/otzaria-voice-he-v3.zip"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum <"$1" | cut -d' ' -f1
  else shasum -a 256 <"$1" | cut -d' ' -f1; fi
}

size() { wc -c <"$1" | tr -d ' '; }

valid() {
  [ -f "$zip" ] && [ "$(size "$zip")" = "$PACK_SIZE" ] && [ "$(sha256 "$zip")" = "$PACK_SHA256" ]
}

mkdir -p "$cache"
if valid; then
  echo "voice pack: using cached $zip"
else
  echo "voice pack: downloading $PACK_URL"
  rm -f "$zip" "$zip.part"
  curl -fL --retry 5 --retry-delay 5 -o "$zip.part" "$PACK_URL"
  mv "$zip.part" "$zip"
  valid || { echo "voice pack: size or SHA-256 mismatch ($(size "$zip") bytes, $(sha256 "$zip"))" >&2; exit 1; }
fi
echo "voice pack: SHA-256 verified ($PACK_SHA256)"

rm -rf "$out"
mkdir -p "$out"
if command -v unzip >/dev/null 2>&1; then
  unzip -q "$zip" -d "$out"
else
  py=python3; command -v python3 >/dev/null 2>&1 || py=python
  "$py" -m zipfile -e "$zip" "$out"
fi
for f in "${PACK_FILES[@]}"; do
  [ -f "$out/$f" ] || { echo "voice pack: missing $f" >&2; exit 1; }
done
printf '%s' "$PACK_VERSION" >"$out/version.txt"
echo "voice pack: unpacked to $out"
