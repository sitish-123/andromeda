#!/usr/bin/env bash
# The shell delegates the lock to the lock process (`noctalia lock`, .agent/TASK-lock-process.md).
# S1: `noctalia msg session lock` locks through org.andromeda.Lock1; the shell reports locked.
# S2: the shell killed with SIGSEGV while locked takes nothing with it: the compositor sees no new lock request, and a
# restarted shell neither races the lock process for the lock nor shows unlocked. A face match unlocks, and the
# shell reports unlocked. Writes noctalia*.log, lock.log, andromeda.log, mock.log and calls.txt to $OUT
# (default ./artifacts/lock-shell-crash).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-shell-crash}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
printf '\n[lockscreen]\nface = true\nface_on_request = false\n' >> "$RUNTIME/home/.config/noctalia/config.toml"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  fail() { echo "FAIL: $*" >&2; grim "$OUT/fail.png" 2> /dev/null || true; exit 1; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  unlocked() { msg status | grep -q "\"locked\": false"; }
  count() { grep -c "$1" "$OUT/andromeda.log" || true; }
  reached() { [[ $(count "$1") -ge $2 ]]; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze "$@" \
    > /dev/null; }
  verifying() { grep -q "^VerifyStartFor" "$OUT/calls.txt" 2> /dev/null; }
  served() { busctl --user status org.andromeda.Lock1 > /dev/null 2>&1; }
  delegating() { grep -q "delegates every lock" "$1"; }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze
  wtype -s 3600000 &
  sleep 0.3
  "$NOCTALIA" lock > "$OUT/lock.log" 2>&1 &
  wait_for "org.andromeda.Lock1 is not served" served
  "$NOCTALIA" > "$OUT/noctalia-1.log" 2>&1 &
  shell=$!
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  wait_for "S1: the shell does not delegate" delegating "$OUT/noctalia-1.log"

  [[ $(msg session lock) == ok ]] || fail "S1: lock refused"
  wait_for "S1: the session did not lock" reached "session is locked" 1
  wait_for "S1: the shell does not report locked" locked
  grep -q "lock requested (user)" "$OUT/lock.log" || fail "S1: the lock process did not take the lock"
  echo "S1 pass: the shell locks through the lock process"

  requests=$(count "session lock requested")
  kill -SEGV "$shell"; wait "$shell" 2> /dev/null || true
  "$NOCTALIA" > "$OUT/noctalia-2.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  wait_for "S2: the restarted shell does not delegate" delegating "$OUT/noctalia-2.log"
  wait_for "S2: the restarted shell does not report locked" locked
  sleep 1
  [[ $(count "session lock requested") == "$requests" ]] || fail "S2: the restarted shell asked for a second lock"
  ! grep -q "denying session lock" "$OUT/andromeda.log" || fail "S2: the compositor denied a racing lock"
  wait_for "S2: face verify did not start" verifying
  gaze Finish sssb verify-match usable usable true
  wait_for "S2: a face match did not unlock" reached "session unlocked" 1
  wait_for "S2: the shell does not report unlocked" unlocked
  echo "S2 pass: a shell crash leaves the lock to the lock process"
  echo PASS
'
