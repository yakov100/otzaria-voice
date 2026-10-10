#!/bin/sh
# Installs the Otzaria voice service for the current user only (no root):
#   ~/.local/lib/otzaria-voice      the program and ONNX Runtime
#   ~/.local/share/otzaria-voice    the voice pack, when this archive has one
#   ~/.local/bin/otzaria-voice      launcher
#   ~/.config/autostart             start at login
# Run from the unpacked archive:  ./install.sh      To remove:  ./install.sh --uninstall
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
config="${XDG_CONFIG_HOME:-$HOME/.config}"
libdir="$HOME/.local/lib/otzaria-voice"
packdir="$data/otzaria-voice/pack"
launcher="$HOME/.local/bin/otzaria-voice"
desktop="$config/autostart/org.otzaria.voice.desktop"

if [ "${1:-}" = "--uninstall" ]; then
  pkill -f "$libdir/otzaria-voice" 2>/dev/null || true
  rm -rf "$libdir" "$data/otzaria-voice"
  rm -f "$launcher" "$desktop"
  echo "שירות הדיבור הוסר."
  exit 0
fi

pkill -f "$libdir/otzaria-voice" 2>/dev/null || true
mkdir -p "$libdir" "$(dirname "$launcher")" "$(dirname "$desktop")"
cp -a "$here/lib/." "$libdir/"
chmod 755 "$libdir/otzaria-voice"

pack=""
if [ -d "$here/pack" ]; then
  rm -rf "$packdir"
  mkdir -p "$(dirname "$packdir")"
  cp -a "$here/pack" "$packdir"
  pack="$packdir"
fi

sed -e "s|@LIBDIR@|$libdir|" -e "s|@PACKDIR@|$pack|" "$here/share/launcher.in" >"$launcher"
chmod 755 "$launcher"
sed -e "s|@LAUNCHER@|$launcher|" "$here/share/otzaria-voice.desktop.in" >"$desktop"

nohup "$launcher" --serve >/dev/null 2>&1 &
echo "שירות הדיבור הותקן ופועל. מעכשיו הוא יעלה מעצמו בכל כניסה למחשב."
