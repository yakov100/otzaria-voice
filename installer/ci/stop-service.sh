#!/usr/bin/env bash
# Makes sure no test service is left answering on a port before the next test
# starts there (Windows CI): asks it to quit, waits up to 10 s, then ends every
# otzaria-voice process. Exits 1 if the port still answers.
#
#   stop-service.sh [port]   (default 47899, the smoke test's)
port="${1:-47899}"
h=(-H 'X-Otzaria-Voice: 1')
s="http://127.0.0.1:$port"
curl -s "${h[@]}" -X POST "$s/quit" >/dev/null 2>&1 || true
for _ in $(seq 1 20); do
  curl -sf "${h[@]}" "$s/status" >/dev/null 2>&1 || exit 0
  sleep 0.5
done
taskkill //F //IM otzaria-voice.exe >/dev/null 2>&1 || true
taskkill //F //IM otzaria-voice-arm64.exe >/dev/null 2>&1 || true
sleep 1
if curl -sf "${h[@]}" "$s/status" >/dev/null 2>&1; then
  echo "::error::port $port still answers"
  exit 1
fi
