#!/usr/bin/env bash
# Packages the macOS build of the Otzaria voice service as an (unsigned) .pkg.
#
#   package.sh --bin <otzaria-voice> --version <x.y.z> --arch <arm64>
#              --ort <onnxruntime dir> --out <dir> [--pack <pack dir>]
#
# Installs:
#   /Applications/Otzaria Voice.app      the service as a background app bundle
#       Contents/MacOS/otzaria-voice     (an app bundle so macOS can ask for
#       Contents/Frameworks/libonnxruntime*.dylib    microphone permission)
#   /Library/LaunchAgents/org.otzaria.voice.plist    start at login: "--serve"
#   /Library/Application Support/OtzariaVoice/pack/  voice pack (with-models only;
#                                    the LaunchAgent sets OTZARIA_VOICE_PACK_DIR)
#   /Library/Application Support/OtzariaVoice/uninstall.sh
#
# The bundle is ad-hoc signed only (no Apple Developer ID): the first time,
# open the .pkg with right-click > Open (or System Settings > Privacy &
# Security > Open Anyway).
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
variant=without-models
[ -n "$pack" ] && variant=with-models
name="otzaria-voice-macos-$arch-$variant-$version"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
payload="$stage/root"
app="$payload/Applications/Otzaria Voice.app"
support="$payload/Library/Application Support/OtzariaVoice"

mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources" \
  "$payload/Library/LaunchAgents" "$support"
install -m755 "$bin" "$app/Contents/MacOS/otzaria-voice"
dylib="$(cd "$ort/lib" && ls libonnxruntime.*.*.*.dylib | head -n1)"
install -m755 "$ort/lib/$dylib" "$app/Contents/Frameworks/$dylib"
ln -sf "$dylib" "$app/Contents/Frameworks/libonnxruntime.1.dylib"
ln -sf "$dylib" "$app/Contents/Frameworks/libonnxruntime.dylib"
sed -e "s|@VERSION@|$version|g" "$here/Info.plist.in" >"$app/Contents/Info.plist"
{
  cat "$root/LICENSE"
  echo
  echo "------------------------------------------------------------------"
  echo "ONNX Runtime (Microsoft) - bundled shared library"
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
} >"$app/Contents/Resources/LICENSES.txt"

pack_env=""
if [ -n "$pack" ]; then
  cp -a "$pack" "$support/pack"
  pack_env="    <key>OTZARIA_VOICE_PACK_DIR</key>
    <string>/Library/Application Support/OtzariaVoice/pack</string>"
fi
# Replace the @PACK_ENV@ line (empty for the without-models variant).
awk -v env="$pack_env" '{ if ($0 == "@PACK_ENV@") { if (env != "") print env } else print }' \
  "$here/org.otzaria.voice.plist.in" >"$payload/Library/LaunchAgents/org.otzaria.voice.plist"
plutil -lint "$payload/Library/LaunchAgents/org.otzaria.voice.plist" "$app/Contents/Info.plist"
install -m755 "$here/uninstall.sh" "$support/uninstall.sh"

# Ad-hoc signature: required for arm64 code once the bundle is assembled.
codesign --force --deep --sign - "$app"

scripts="$stage/scripts"
mkdir -p "$scripts"
install -m755 "$here/scripts/preinstall" "$here/scripts/postinstall" "$scripts/"

# Keep the app in /Applications even if a copy exists elsewhere.
pkgbuild --analyze --root "$payload" "$stage/component.plist" >/dev/null
n="$(plutil -convert xml1 -o - "$stage/component.plist" | grep -c '<key>RootRelativeBundlePath</key>' || true)"
for ((i = 0; i < n; i++)); do
  plutil -replace "$i.BundleIsRelocatable" -bool NO "$stage/component.plist"
done

pkgbuild --root "$payload" --component-plist "$stage/component.plist" --scripts "$scripts" \
  --identifier org.otzaria.voice --version "$version" --install-location / \
  "$stage/component.pkg" >/dev/null
productbuild --package "$stage/component.pkg" --identifier org.otzaria.voice.installer \
  --version "$version" "$out/$name.pkg" >/dev/null
echo "built $out/$name.pkg"
