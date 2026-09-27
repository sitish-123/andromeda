#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.Inhibit: an Inhibit call with the idle and suspend flags takes logind
# idle and sleep block inhibitors (a mock login1 on the private system bus records them), Request.Close releases
# both, and CreateMonitor answers 0 and emits StateChanged with session-state 1. Writes calls.txt and
# signals.txt to $OUT (default ./artifacts/inhibit-portal).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/inhibit-portal}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  portal() { gdbus call --session --dest org.freedesktop.impl.portal.desktop.noctalia "$@"; }
  wait_line() { for _ in $(seq 30); do grep -qx "$1" "$OUT/calls.txt" && return 0; sleep 0.1; done
    echo "FAIL: logind never saw \"$1\"; calls: $(tr "\n" "," < "$OUT/calls.txt")"; exit 1; }

  req=/org/freedesktop/portal/desktop/request/1_1/e2e
  portal --object-path /org/freedesktop/portal/desktop --method org.freedesktop.impl.portal.Inhibit.Inhibit \
    "$req" e2e.app "" 12 "{}" > /dev/null
  wait_line "Inhibit idle e2e.app block"
  wait_line "Inhibit sleep e2e.app block"
  grep -q "^Release" "$OUT/calls.txt" && { echo "FAIL: released before Close"; exit 1; }
  portal --object-path "$req" --method org.freedesktop.impl.portal.Request.Close > /dev/null
  wait_line "Release idle e2e.app"
  wait_line "Release sleep e2e.app"

  gdbus monitor --session --dest org.freedesktop.impl.portal.desktop.noctalia > "$OUT/signals.txt" &
  sleep 0.3
  response=$(portal --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.impl.portal.Inhibit.CreateMonitor \
    /org/freedesktop/portal/desktop/request/1_1/m /org/freedesktop/portal/desktop/session/1_1/m e2e.app "")
  [[ $response == "(uint32 0,)" ]] || { echo "FAIL: CreateMonitor answered $response"; exit 1; }
  for _ in $(seq 30); do grep -q "StateChanged" "$OUT/signals.txt" && break; sleep 0.1; done
  grep -A1 "StateChanged" "$OUT/signals.txt" | grep -q "session-state.*uint32 1" \
    || { echo "FAIL: no StateChanged running; signals: $(cat "$OUT/signals.txt")"; exit 1; }
  kill %2 %3 2>/dev/null || true
'
cat "$OUT/calls.txt"
echo "PASS; artifacts: $OUT"
