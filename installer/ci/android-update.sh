#!/usr/bin/env bash
# Android emulator check (CI only) that a newer APK installs over an older one
# as an update: installs <old.apk>, leaves a file in the app's data, installs
# <new.apk> over it without uninstalling, and checks that the app was updated
# (same first-install time) and the file is still there. The two APKs of one
# run are signed with the same key and have the same versionCode, like two
# releases signed with the project's key (service/android/SIGNING.md).
#
#   android-update.sh <old.apk> <new.apk>
set -euo pipefail
old="$1"
new="$2"
pkg=org.otzaria.voice

fail() { echo "::error::android update test: $*"; exit 1; }
# awk reads to the end: an early exit (grep -m1) would kill adb with SIGPIPE under pipefail.
first_install() { adb shell dumpsys package "$pkg" | awk '/firstInstallTime/ && !n++ { gsub(/[\r ]/, ""); print }'; }

adb uninstall "$pkg" >/dev/null 2>&1 || true
adb install "$old"
adb shell "run-as $pkg sh -c 'mkdir -p files/stt && echo kept > files/stt/update-check'"
before="$(first_install)"
[ -n "$before" ] || fail "no firstInstallTime after installing $old"

# -r replaces the installed app; no -d (downgrade) and no uninstall.
adb install -r "$new" || fail "Android refused to install $new over $old"
after="$(first_install)"
[ "$after" = "$before" ] || fail "the app was installed anew ($before -> $after), not updated"
[ "$(adb shell "run-as $pkg cat files/stt/update-check" | tr -d '\r')" = kept ] || fail "the update deleted the app's files"
echo "update over the installed APK kept the app's files ($after)"
