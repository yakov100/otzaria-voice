#!/bin/sh
# Removes the Otzaria voice service. Run:
#   sudo "/Library/Application Support/OtzariaVoice/uninstall.sh"
set -u
user="$(stat -f %Su /dev/console 2>/dev/null)"
if [ -n "$user" ] && [ "$user" != "root" ]; then
  launchctl bootout "gui/$(id -u "$user")/org.otzaria.voice" 2>/dev/null || true
fi
rm -f /Library/LaunchAgents/org.otzaria.voice.plist
rm -rf "/Applications/Otzaria Voice.app" "/Library/Application Support/OtzariaVoice"
pkgutil --forget org.otzaria.voice >/dev/null 2>&1 || true
echo "שירות הדיבור הוסר."
