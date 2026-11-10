#!/usr/bin/env bash
# PrepareForSleep on an unlocked session locks it and the lock is ready for sleep within 700 ms: the enter transition
# (1.5 s) is skipped, because nobody sees it and the suspend waits for it. Writes noctalia.log and mock.log to $OUT
# (default ./artifacts/lock-sleep-ready).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-sleep-ready}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  logged() { grep -q "$1" "$OUT/noctalia.log"; }
  stamp() { grep -m1 "$1" "$OUT/noctalia.log" | grep -oE "^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}" \
    | awk -F: "{ print (\$1 * 3600 + \$2 * 60 + \$3) * 1000 }"; }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status org.freedesktop.login1 \
    > /dev/null 2>&1
  ANDROMEDA_LOG_LEVEL=debug "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  wtype -s 3600000 &
  sleep 0.3

  busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /org/freedesktop/login1 dsk.test.Gaze Sleep b true
  wait_for "sleep did not lock" logged "session lock requested"
  wait_for "the lock never became ready for sleep" logged "lock screen ready for sleep"
  ms=$(( $(stamp "lock screen ready for sleep") - $(stamp "session lock requested") ))
  echo "lock requested -> ready for sleep: ${ms} ms"
  (( ms < 700 )) || fail "the suspend waited ${ms} ms for the lock"
  echo PASS
'
