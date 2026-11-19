#!/usr/bin/env bash
# Gets official ONNX Runtime 1.28.0 for one platform from Microsoft's NuGet
# package Microsoft.ML.OnnxRuntime, verifies the pinned SHA-256 of the package,
# and lays out the files the installers read (lib/ and the license files) under
# the names the release archive of the same version gave them. Prints that
# directory (it contains lib/, include/, LICENSE, ThirdPartyNotices.txt). The
# archive also had files the package lacks and nothing here reads: lib/cmake,
# lib/pkgconfig, include/core/providers/*.h, VERSION_NUMBER, GIT_COMMIT_ID,
# README.md, Privacy.md.
#
#   fetch-ort.sh <linux-x64|linux-aarch64|osx-arm64|win-x64|win-arm64> <out-dir>
#
# win-x64 and win-arm64 are used only by CI's tests: on users' Windows
# machines the service uses the copy that comes with Otzaria.
#
# Why NuGet: until 2026-11 this downloaded the release archives from
# github.com/microsoft/onnxruntime, which answered 404 when checked on
# 2026-11-19 (reports date it from 2026-11-18).
# The package is the same build (commit da9b5e3, in its .nuspec). Checked on
# 2026-11-19 against the archives (or against the installers of service 2.5.3,
# built from them): the libraries of linux-x64, linux-aarch64 and win-x64 are
# byte-identical, osx-arm64 differs only in its code signature (the macOS
# installer re-signs it anyway), and LICENSE, ThirdPartyNotices.txt and the
# headers are the same text with CRLF line ends. The package's author (Microsoft) and
# repository (nuget.org) signatures were verified with nuget.exe 8.0.0, and its
# SHA-512 matches the packageHash in nuget.org's catalog.
#
# Environment:
#   ORT_CACHE               directory that keeps the package between runs
#                           (default: <out-dir>, and the package is deleted
#                           after unpacking). A cached copy is checked against
#                           the size and SHA-256 below before every use, and
#                           downloaded again from nuget.org if it does not match.
#   ORT_VERIFY_SIGNATURE=1  also verify both signatures with
#                           `dotnet nuget verify --all`; fails if dotnet is missing.
#
# ONNX Runtime 1.28.0 ships no x86_64/universal macOS build, so macOS is arm64
# (Apple silicon) only.
set -euo pipefail

ORT_VERSION=1.28.0
NUPKG="microsoft.ml.onnxruntime.$ORT_VERSION.nupkg"
NUPKG_URL="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$ORT_VERSION/$NUPKG"
NUPKG_SIZE=139145017
NUPKG_SHA256=769d1d3ea8ab6cd69f737c9dd4d4462aa4ad0ccfa106eaf506efc40d7bead5db

[ $# -eq 2 ] || { echo "usage: $0 <platform> <out-dir>" >&2; exit 2; }
platform="$1"
out="$2"

# rid: the folder in the package; lib: the library's name there, and the name
# the release archive gave it.
case "$platform" in
  linux-x64)     rid=linux-x64;   lib=libonnxruntime.so;    archived=libonnxruntime.so.$ORT_VERSION ;;
  linux-aarch64) rid=linux-arm64; lib=libonnxruntime.so;    archived=libonnxruntime.so.$ORT_VERSION ;;
  osx-arm64)     rid=osx-arm64;   lib=libonnxruntime.dylib; archived=libonnxruntime.$ORT_VERSION.dylib ;;
  win-x64)       rid=win-x64;     lib=onnxruntime.dll;      archived=onnxruntime.dll ;;
  win-arm64)     rid=win-arm64;   lib=onnxruntime.dll;      archived=onnxruntime.dll ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

mkdir -p "$out"
cache="${ORT_CACHE:-$out}"
mkdir -p "$cache"
pkg="$cache/$NUPKG"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum <"$1" | cut -d' ' -f1
  else shasum -a 256 <"$1" | cut -d' ' -f1; fi
}
size() { wc -c <"$1" | tr -d ' '; }
matches() { [ -f "$1" ] && [ "$(size "$1")" = "$NUPKG_SIZE" ] && [ "$(sha256 "$1")" = "$NUPKG_SHA256" ]; }

if matches "$pkg"; then
  echo "onnxruntime: using the cached package $pkg" >&2
else
  [ -f "$pkg" ] && echo "onnxruntime: the cached package does not match; downloading it again" >&2
  echo "onnxruntime: downloading $NUPKG_URL" >&2
  part="$(mktemp "$pkg.XXXXXX")"
  trap 'rm -f "$part"' EXIT   # also when interrupted: no partial download stays behind
  curl -fsSL --retry 5 --retry-delay 5 -o "$part" "$NUPKG_URL"
  matches "$part" || {
    echo "onnxruntime: size or SHA-256 mismatch for $NUPKG ($(size "$part") bytes, $(sha256 "$part"))" >&2
    exit 1; }
  mv -f "$part" "$pkg"
  trap - EXIT
fi
echo "onnxruntime: SHA-256 verified ($NUPKG_SHA256)" >&2

if [ "${ORT_VERIFY_SIGNATURE:-0}" = 1 ]; then
  command -v dotnet >/dev/null 2>&1 || { echo "onnxruntime: ORT_VERIFY_SIGNATURE=1 needs dotnet" >&2; exit 1; }
  log="$(DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 dotnet nuget verify --all --verbosity detailed "$pkg" 2>&1)" || {
    echo "$log" >&2; echo "onnxruntime: signature verification failed" >&2; exit 1; }
  echo "$log" >&2
  # Both signatures, each by Microsoft: the author's and nuget.org's.
  for want in 'Signature type: Author' 'CN=Microsoft Corporation,' \
              'Signature type: Repository' 'CN=NuGet.org Repository by Microsoft,'; do
    grep -qF "$want" <<<"$log" || { echo "onnxruntime: signature check: '$want' not found" >&2; exit 1; }
  done
  echo "onnxruntime: author and repository signatures verified" >&2
fi

# Unpack only what is needed, into a fresh folder named like the archive's.
dir="$out/onnxruntime-$platform-$ORT_VERSION"
tmp="$out/.nupkg-$platform"
rm -rf "$dir" "$tmp"
mkdir -p "$tmp"
if command -v unzip >/dev/null 2>&1; then
  unzip -q -o "$pkg" "runtimes/$rid/native/*" "build/native/include/*" LICENSE ThirdPartyNotices.txt -d "$tmp" >&2
else
  py=python3; command -v python3 >/dev/null 2>&1 || py=python
  "$py" -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
keep = ("runtimes/%s/native/" % sys.argv[3], "build/native/include/")
for n in z.namelist():
    if n.startswith(keep) or n in ("LICENSE", "ThirdPartyNotices.txt"):
        z.extract(n, sys.argv[2])
' "$pkg" "$tmp" "$rid"
fi
[ -f "$tmp/runtimes/$rid/native/$lib" ] || { echo "onnxruntime: $rid/$lib missing in the package" >&2; exit 1; }

mkdir -p "$dir/lib" "$dir/include"
cp "$tmp/runtimes/$rid/native/"* "$dir/lib/"
cp "$tmp/build/native/include/"* "$dir/include/"
case "$platform" in
  win-*)
    # The zip carried these with CRLF line ends, as the package does.
    cp "$tmp/LICENSE" "$tmp/ThirdPartyNotices.txt" "$dir/" ;;
  *)
    # The tgz carried these and the headers with LF line ends.
    for f in LICENSE ThirdPartyNotices.txt; do tr -d '\r' <"$tmp/$f" >"$dir/$f"; done
    for f in "$dir/include/"*; do tr -d '\r' <"$f" >"$f.lf"; mv -f "$f.lf" "$f"; done
    mv "$dir/lib/$lib" "$dir/lib/$archived"
    chmod 755 "$dir/lib/$archived"
    case "$platform" in
      linux-*)
        ln -sf "$archived" "$dir/lib/libonnxruntime.so.1"
        ln -sf libonnxruntime.so.1 "$dir/lib/libonnxruntime.so" ;;
      osx-*)
        ln -sf "$archived" "$dir/lib/libonnxruntime.dylib" ;;
    esac ;;
esac
rm -rf "$tmp"
[ -n "${ORT_CACHE:-}" ] || rm -f "$pkg"
echo "onnxruntime: $platform: lib/$archived ($(sha256 "$dir/lib/$archived"))" >&2
echo "$dir"
