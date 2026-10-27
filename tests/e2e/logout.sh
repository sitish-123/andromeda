#!/usr/bin/env bash
# Logout under AddressSanitizer: a headless Andromeda with two outputs, three windows, and the shell quits through
# `session-quit` while systemd-style kills hit the windows at the same moment, the way logging out of a real session
# does. The compositor must exit 0 with no ASan report. Writes andromeda.log and result.txt to $OUT (default
# ./artifacts/logout).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/logout}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ANDROMEDA=${ANDROMEDA_ASAN:-$ROOT/compositor/build-asan/andromeda}
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
source "$(dirname "$0")/lib.sh"
boot_headless 2
compositor=$(jobs -p | tail -1)

with_noctalia '
  pids=""
  for i in 1 2 3; do foot --config=/dev/null --title=w$i sh -c "sleep 60" > /dev/null 2>&1 & pids="$pids $!"; done
  for _ in $(seq 50); do [[ $("$ANDROMEDA" windows --json | jq length) -eq 3 ]] && break; sleep 0.1; done
  "$ANDROMEDA" msg workspace-switch:2 > /dev/null
  "$ANDROMEDA" msg session-quit:skip-confirmation > /dev/null &
  kill -TERM $pids 2> /dev/null || true
  for _ in $(seq 100); do kill -0 '"$compositor"' 2> /dev/null || break; sleep 0.1; done
'
status=0
wait "$compositor" || status=$?
if grep -q 'ERROR: AddressSanitizer' "$OUT/andromeda.log" || ((status != 0)); then
  echo "FAIL exit=$status" | tee "$OUT/result.txt"
  grep -m1 -A20 'ERROR: AddressSanitizer' "$OUT/andromeda.log" || true
  exit 1
fi
echo "PASS exit=0, no ASan report" | tee "$OUT/result.txt"
