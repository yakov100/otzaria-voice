#!/usr/bin/env bash
# Packages the Linux build of the Otzaria voice service as a .deb and a
# per-user .tar.gz (with install.sh).
#
#   package.sh --bin <otzaria-voice> --version <x.y.z> --arch <x64|arm64>
#              --ort <onnxruntime dir> --out <dir> [--pack <pack dir>]
#
# --pack is a directory from installer/fetch-pack.sh; with it the packages are
# the "with-models" variant and carry the voice pack.
#
# .deb layout:
#   /usr/lib/otzaria-voice/          otzaria-voice + libonnxruntime.so*
#   /usr/bin/otzaria-voice           launcher (sets LD_LIBRARY_PATH and, with
#                                    models, OTZARIA_VOICE_PACK_DIR)
#   /usr/share/otzaria-voice/pack/   voice pack (with-models only)
#   /etc/xdg/autostart/org.otzaria.voice.desktop   start at login: "otzaria-voice --serve"
#   /usr/share/doc/otzaria-voice/copyright
set -euo pipefail

bin="" version="" arch="" ort="" out="" pack=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin) bin="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --arch) arch="$2"; shift 2 ;;
    --ort) ort="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --pack) pack="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$bin" ] && [ -n "$version" ] && [ -n "$arch" ] && [ -n "$ort" ] && [ -n "$out" ] ||
  { echo "missing arguments (see the header of $0)" >&2; exit 2; }

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
case "$arch" in
  x64) debarch=amd64 ;;
  arm64) debarch=arm64 ;;
  *) echo "unknown arch: $arch" >&2; exit 2 ;;
esac
variant=without-models
[ -n "$pack" ] && variant=with-models
name="otzaria-voice-linux-$arch-$variant-$version"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# Copies the program and ONNX Runtime into $1.
copy_libs() {
  install -Dm755 "$bin" "$1/otzaria-voice"
  local so
  so="$(cd "$ort/lib" && ls libonnxruntime.so.*.*.* | head -n1)"
  install -m644 "$ort/lib/$so" "$1/$so"
  ln -sf "$so" "$1/libonnxruntime.so.1"
  ln -sf "libonnxruntime.so.1" "$1/libonnxruntime.so"
  if [ -f "$ort/lib/libonnxruntime_providers_shared.so" ]; then
    install -m644 "$ort/lib/libonnxruntime_providers_shared.so" "$1/"
  fi
}

# Writes the combined license text to $1.
notices() {
  {
    echo "Otzaria voice service"
    echo "Source: https://github.com/yakov100/otzaria-voice"
    echo
    cat "$root/LICENSE"
    echo
    echo "------------------------------------------------------------------"
    echo "ONNX Runtime $(basename "$ort" | sed 's/.*-//') (Microsoft) - bundled shared library"
    echo "------------------------------------------------------------------"
    cat "$ort/LICENSE"
    if [ -f "$ort/ThirdPartyNotices.txt" ]; then echo; cat "$ort/ThirdPartyNotices.txt"; fi
    if [ -n "$pack" ]; then
      echo
      echo "------------------------------------------------------------------"
      echo "Hebrew voice pack (speech model and language data)"
      echo "------------------------------------------------------------------"
      cat "$pack/LICENSES.txt"
    fi
  } >"$1"
}

# ---------------------------------------------------------------- .deb
pkg="$stage/deb"
copy_libs "$pkg/usr/lib/otzaria-voice"
packdir=""
if [ -n "$pack" ]; then
  packdir="/usr/share/otzaria-voice/pack"
  mkdir -p "$pkg$packdir"
  cp -a "$pack/." "$pkg$packdir/"
  find "$pkg$packdir" -type d -exec chmod 755 {} + && find "$pkg$packdir" -type f -exec chmod 644 {} +
fi
mkdir -p "$pkg/usr/bin" "$pkg/etc/xdg/autostart" "$pkg/usr/share/doc/otzaria-voice" "$pkg/DEBIAN"
sed -e "s|@LIBDIR@|/usr/lib/otzaria-voice|" -e "s|@PACKDIR@|$packdir|" "$here/launcher.in" >"$pkg/usr/bin/otzaria-voice"
chmod 755 "$pkg/usr/bin/otzaria-voice"
sed -e "s|@LAUNCHER@|/usr/bin/otzaria-voice|" "$here/otzaria-voice.desktop.in" >"$pkg/etc/xdg/autostart/org.otzaria.voice.desktop"
chmod 644 "$pkg/etc/xdg/autostart/org.otzaria.voice.desktop"
notices "$pkg/usr/share/doc/otzaria-voice/copyright"

# Shared-library dependencies of the program itself (ONNX Runtime is bundled).
depends=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
  mkdir -p "$stage/shlib/debian"
  printf 'Source: otzaria-voice\n\nPackage: otzaria-voice\nArchitecture: any\n' >"$stage/shlib/debian/control"
  depends="$(cd "$stage/shlib" && dpkg-shlibdeps -O --ignore-missing-info \
    -l"$pkg/usr/lib/otzaria-voice" "$pkg/usr/lib/otzaria-voice/otzaria-voice" 2>/dev/null |
    sed -n 's/^shlibs:Depends=//p')" || depends=""
fi
installed_kb="$(du -sk "$pkg" | cut -f1)"
{
  echo "Package: otzaria-voice"
  echo "Version: $version"
  echo "Architecture: $debarch"
  echo "Maintainer: yakov100 <https://github.com/yakov100>"
  echo "Installed-Size: $installed_kb"
  [ -n "$depends" ] && echo "Depends: $depends"
  echo "Section: sound"
  echo "Priority: optional"
  echo "Homepage: https://github.com/yakov100/otzaria-voice"
  echo "Description: Local Hebrew speech recognition for Otzaria plugins"
  echo " Listens on http://127.0.0.1:47811 only and recognizes Hebrew speech on"
  echo " this computer, with no internet connection, for the Otzaria voice plugin."
  if [ -n "$pack" ]; then echo " This package includes the Hebrew voice pack (speech models)."
  else echo " The voice pack (about 330 MB) is downloaded on first use."; fi
} >"$pkg/DEBIAN/control"
dpkg-deb --root-owner-group -Zgzip --build "$pkg" "$out/$name.deb" >/dev/null
echo "built $out/$name.deb"

# ---------------------------------------------------------------- .tar.gz
tdir="$stage/tar/otzaria-voice"
copy_libs "$tdir/lib"
mkdir -p "$tdir/share"
cp "$here/launcher.in" "$here/otzaria-voice.desktop.in" "$tdir/share/"
install -m755 "$here/install.sh" "$tdir/install.sh"
[ -n "$pack" ] && cp -a "$pack" "$tdir/pack"
notices "$tdir/LICENSES.txt"
tar -C "$stage/tar" -czf "$out/$name.tar.gz" otzaria-voice
echo "built $out/$name.tar.gz"
