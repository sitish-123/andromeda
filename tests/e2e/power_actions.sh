#!/usr/bin/env bash
# Session power actions reach logind directly: a mock org.freedesktop.login1 on the test's private system bus records
# each call while `noctalia msg session suspend|reboot|shutdown|hibernate|suspend-then-hibernate` runs. Hibernate and
# suspend-then-hibernate are also gated on CanHibernate/CanSuspendThenHibernate: with the mock reporting "no", the
# call to logind must never happen. Writes calls.txt to $OUT (default ./artifacts/power-actions).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/power-actions}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
with_noctalia '
  start_mock() {
    : > "$OUT/mock.log"
    python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
    mock=$!
    for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
    grep -q ready "$OUT/mock.log" || { echo "FAIL: mock logind did not start: $(cat "$OUT/mock.log")"; exit 1; }
  }
  expect() {
    local action=$1 want=$2
    "$NOCTALIA" msg session "$action" > /dev/null
    for _ in $(seq 50); do grep -qx "$want" "$OUT/calls.txt" && return 0; sleep 0.1; done
    echo "FAIL $action: logind never received \"$want\"; calls: $(tr "\n" "," < "$OUT/calls.txt")"
    exit 1
  }
  expect_no_call() {
    local action=$1 forbidden=$2
    local log_from calls_from
    log_from=$(wc -l < "$OUT/noctalia.log")
    calls_from=$(wc -l < "$OUT/calls.txt")
    "$NOCTALIA" msg session "$action" > /dev/null
    sleep 0.3
    tail -n +"$((calls_from + 1))" "$OUT/calls.txt" | grep -qx "$forbidden" \
      && { echo "FAIL $action: logind received \"$forbidden\" despite CanX=no"; exit 1; }
    tail -n +"$((log_from + 1))" "$OUT/noctalia.log" | grep -q "unavailable" \
      || { echo "FAIL $action: no gating warning logged"; exit 1; }
  }

  start_mock
  expect suspend "Suspend True"
  expect reboot "Reboot True"
  expect shutdown "PowerOff True"
  expect hibernate "Hibernate True"
  expect suspend-then-hibernate "SuspendThenHibernate True"
  kill "$mock"; wait "$mock" 2>/dev/null || true

  MOCK_CAN_HIBERNATE=no MOCK_CAN_SUSPEND_THEN_HIBERNATE=no start_mock
  expect_no_call hibernate "Hibernate True"
  expect_no_call suspend-then-hibernate "SuspendThenHibernate True"
  kill "$mock"
'
echo "PASS: $(tr '\n' ',' < "$OUT/calls.txt"); artifacts: $OUT"
