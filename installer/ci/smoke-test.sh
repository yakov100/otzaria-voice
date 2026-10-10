#!/usr/bin/env bash
# End-to-end check of an installed service, used by CI on every platform:
# starts it on a spare port with a generated test recording instead of the
# microphone (OTZARIA_VOICE_TEST_WAV), checks /status (ONNX Runtime and the
# voice pack found), runs one dictation (the model loads and decodes; the
# recording is quiet, so the text may be empty) and asks the service to quit.
#
#   smoke-test.sh <service executable or launcher>
#
# OTZARIA_ORT_PATH may be set by the caller (Windows CI has no Otzaria install).
set -euo pipefail

exe="$1"
port=47899
base="http://127.0.0.1:$port"
work="$(mktemp -d)"
py=python3
command -v python3 >/dev/null 2>&1 && python3 -c '' 2>/dev/null || py=python

# 1.5 s of faint noise, 16 kHz mono PCM16.
"$py" - "$work/test.wav" <<'EOF'
import random, struct, sys, wave
random.seed(1)
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(b"".join(struct.pack("<h", random.randint(-60, 60)) for _ in range(24000)))
EOF
wav="$work/test.wav"
command -v cygpath >/dev/null 2>&1 && wav="$(cygpath -w "$wav")"

export OTZARIA_VOICE_PORT=$port OTZARIA_VOICE_TEST_WAV="$wav"
"$exe" --serve >"$work/service.out" 2>&1 &
pid=$!

hdr=(-H "X-Otzaria-Voice: 1")
status=""
for _ in $(seq 1 30); do
  status="$(curl -sf "${hdr[@]}" "$base/status" || true)"
  [ -n "$status" ] && break
  sleep 1
done
echo "status: $status"
fail() { echo "::error::smoke test: $*"; cat "$work/service.out" || true; kill "$pid" 2>/dev/null || true; exit 1; }
[ -n "$status" ] || fail "the service did not answer on $base"
grep -q '"runtime":true' <<<"$status" || fail "ONNX Runtime not found"
grep -q '"pack":true' <<<"$status" || fail "voice pack not found"

curl -sN "${hdr[@]}" -H "Content-Type: application/json" --max-time 180 \
  -d '{"noSpeechMs":1500,"endAfterSilenceMs":1500}' "$base/dictate" >"$work/dictate.out" || true
echo "dictation:"; cat "$work/dictate.out"
grep -q '"type":"ready"' "$work/dictate.out" || fail "the model did not load"
grep -q '"type":"error"' "$work/dictate.out" && fail "dictation reported an error"
grep -q '"type":"end"' "$work/dictate.out" || fail "dictation did not end"

curl -sf "${hdr[@]}" -X POST "$base/quit" >/dev/null || true
for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
kill "$pid" 2>/dev/null || true

# The offline evaluation runs the whole decoder (model + language model) over a
# louder recording, whatever it hears.
"$py" - "$work/loud.wav" <<'EOF'
import math, random, struct, sys, wave
random.seed(2)
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(i * 0.07) * (0.5 + random.random())))
                           for i in range(32000)))
EOF
printf '{"id": "smoke", "wav": "loud.wav", "ref": "", "tags": [], "split": "dev"}\n' >"$work/manifest.jsonl"
m="$work/manifest.jsonl" o="$work/eval.jsonl"
command -v cygpath >/dev/null 2>&1 && { m="$(cygpath -w "$m")"; o="$(cygpath -w "$o")"; }
unset OTZARIA_VOICE_TEST_WAV
"$exe" --eval "$m" "$o" || fail "--eval exited $?"
echo "eval:"; cat "$work/eval.jsonl"
grep -q '"hyp"' "$work/eval.jsonl" || fail "--eval wrote no result"
echo "smoke test passed"
