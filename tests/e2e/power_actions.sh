#!/usr/bin/env bash
# Session power actions reach logind directly: a mock org.freedesktop.login1 on the test's private system bus records
# each call while `noctalia msg session suspend|reboot|shutdown` runs. Writes calls.txt to $OUT (default
# ./artifacts/power-actions).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/power-actions}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock logind did not start: $(cat "$OUT/mock.log")"; exit 1; }
  expect() {
    local action=$1 want=$2
    "$NOCTALIA" msg session "$action" > /dev/null
    for _ in $(seq 50); do grep -qx "$want" "$OUT/calls.txt" && return 0; sleep 0.1; done
    echo "FAIL $action: logind never received \"$want\"; calls: $(tr "\n" "," < "$OUT/calls.txt")"
    exit 1
  }
  expect suspend "Suspend True"
  expect reboot "Reboot True"
  expect shutdown "PowerOff True"
  kill "$mock"
'
echo "PASS: $(tr '\n' ',' < "$OUT/calls.txt"); artifacts: $OUT"
