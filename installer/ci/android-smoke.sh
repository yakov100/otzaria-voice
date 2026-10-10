#!/usr/bin/env bash
# Android emulator check of a built APK (CI only): installs it, grants the
# permissions a user would grant, opens the app (which starts the foreground
# service), then talks to the service through adb port forwarding: /status must
# report ONNX Runtime and the voice pack, and one dictation (the emulator's
# silent microphone) must load the model and end without an error.
#
#   android-smoke.sh <apk>
set -euo pipefail
apk="$1"
pkg=org.otzaria.voice
base=http://127.0.0.1:47811
hdr=(-H "X-Otzaria-Voice: 1")

adb install -r -g "$apk"
adb shell pm grant "$pkg" android.permission.RECORD_AUDIO || true
adb shell pm grant "$pkg" android.permission.POST_NOTIFICATIONS || true
adb shell am start -W -n "$pkg/.MainActivity"
adb forward tcp:47811 tcp:47811

fail() {
  echo "::error::android smoke test: $*"
  adb logcat -d -s OtzariaVoice:V AndroidRuntime:E | tail -n 80 || true
  exit 1
}

# The with-models APK copies its pack out of the APK on first run (~330 MB).
status=""
for _ in $(seq 1 90); do
  status="$(curl -sf "${hdr[@]}" "$base/status" || true)"
  if [ -n "$status" ] && grep -q '"pack":true' <<<"$status"; then break; fi
  sleep 2
done
echo "status: $status"
[ -n "$status" ] || fail "the service did not answer"
grep -q '"runtime":true' <<<"$status" || fail "ONNX Runtime not found"
grep -q '"pack":true' <<<"$status" || fail "voice pack not ready"

curl -sN "${hdr[@]}" -H "Content-Type: application/json" --max-time 240 \
  -d '{"noSpeechMs":2000,"endAfterSilenceMs":1500}' "$base/dictate" >dictate.out || true
echo "dictation:"; cat dictate.out
grep -q '"type":"ready"' dictate.out || fail "the model did not load"
grep -q '"type":"error"' dictate.out && fail "dictation reported an error"
grep -q '"type":"end"' dictate.out || fail "dictation did not end"
echo "android smoke test passed"
