#!/usr/bin/env bash
# Downloads an official ONNX Runtime 1.28.0 release for one platform from
# github.com/microsoft/onnxruntime, verifies the pinned SHA-256, and unpacks it.
# Prints the unpacked directory (it contains lib/, include/, LICENSE,
# ThirdPartyNotices.txt).
#
#   fetch-ort.sh <linux-x64|linux-aarch64|osx-arm64> <out-dir>
#
# The hashes below were computed from the release files themselves (and match
# the digests GitHub publishes for those assets). ONNX Runtime 1.28.0 ships no
# x86_64/universal macOS build, so macOS is arm64 (Apple silicon) only.
set -euo pipefail

ORT_VERSION=1.28.0
[ $# -eq 2 ] || { echo "usage: $0 <platform> <out-dir>" >&2; exit 2; }
platform="$1"
out="$2"

case "$platform" in
  linux-x64)     sha=a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407 ;;
  linux-aarch64) sha=e15ff8b5d85afe6c144d97c6fd432254bf76a219daaf17658087d6ecb3e8f0bb ;;
  osx-arm64)     sha=1268b359718099bde2cedb55787f182a130067bc4f31e8c88478c445b850d3d8 ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

name="onnxruntime-$platform-$ORT_VERSION"
url="https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/$name.tgz"
mkdir -p "$out"
tgz="$out/$name.tgz"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

echo "onnxruntime: downloading $url" >&2
curl -fsSL --retry 5 --retry-delay 5 -o "$tgz" "$url"
got="$(sha256 "$tgz")"
[ "$got" = "$sha" ] || { echo "onnxruntime: SHA-256 mismatch for $name ($got)" >&2; exit 1; }
echo "onnxruntime: SHA-256 verified ($sha)" >&2
tar -xzf "$tgz" -C "$out"
rm -f "$tgz"
echo "$out/$name"
