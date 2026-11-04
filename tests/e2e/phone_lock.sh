#!/usr/bin/env bash
# Lock from the phone (Link): org.andromeda.Shell1.LockFromPhone locks with face unlock and the grace window off, so
# a key press inside a 60 s grace window neither unlocks nor arms face unlock, with gazed present. Writes calls.txt,
# noctalia.log, mock.log to $OUT (default ./artifacts/phone-lock).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/phone-lock}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  MOCK='"$MOCK"'
  CONFIG=$HOME/.config/noctalia/config.toml
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  called() { grep -c "^$1" "$OUT/calls.txt" 2> /dev/null || true; }
  locked() { msg status | grep -q "\"locked\": true"; }

  kill %1; wait %1 2> /dev/null || true
  printf "\n[lockscreen]\ngrace_period_seconds = 60\n" >> "$CONFIG"
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  # A keyboard must exist before the lock surface maps, or the surface never receives keyboard focus.
  wtype -s 3600000 &
  sleep 0.3

  : > "$OUT/calls.txt"
  python3 "$MOCK" "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze \
    > /dev/null 2>&1

  busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.andromeda.Shell1 /org/andromeda/Shell1 \
    org.andromeda.Shell1 LockFromPhone || fail "LockFromPhone failed"
  wait_for "did not lock" locked
  wtype -k Escape
  sleep 1
  locked || fail "a key press inside the grace window unlocked a phone lock"
  [[ $(called Claim) == 0 && $(called VerifyStartFor) == 0 ]] || fail "face unlock ran after a phone lock"
'
echo "PASS: phone lock keeps face and grace off; artifacts: $OUT"
