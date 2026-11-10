#!/usr/bin/env bash
# The lock screen in its own process (`noctalia lock`, .agent/TASK-lock-process.md), with no shell running.
# P1: org.andromeda.Lock1 Lock("user") locks the session and a face match unlocks it.
# P2: a lock process killed with SIGSEGV while locked leaves the compositor's secure blank; a new lock process locks
# again by itself (the compositor's relock) within 1 s of its start, and a face match unlocks.
# Writes lock-*.log, andromeda.log, mock.log and calls.txt to $OUT (default ./artifacts/lock-process-crash).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-process-crash}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
printf '\n[lockscreen]\nface = true\nface_on_request = false\n' >> "$RUNTIME/home/.config/noctalia/config.toml"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  fail() { echo "FAIL: $*" >&2; grim "$OUT/fail.png" 2> /dev/null || true; exit 1; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  count() { grep -c "$1" "$OUT/andromeda.log" || true; }
  reached() { [[ $(count "$1") -ge $2 ]]; }
  ms() { date +%s%3N; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze "$@" \
    > /dev/null; }
  verifying() { [[ $(grep -c "^VerifyStartFor" "$OUT/calls.txt" 2> /dev/null || true) -ge $1 ]]; }
  served() { busctl --user status org.andromeda.Lock1 > /dev/null 2>&1; }
  start_lock() {
    "$NOCTALIA" lock > "$OUT/lock-$1.log" 2>&1 &
    lockpid=$!
  }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze
  # A keyboard must exist before the lock surface maps, or the surface never receives keyboard focus.
  wtype -s 3600000 &
  sleep 0.3

  start_lock 1
  wait_for "P1: org.andromeda.Lock1 is not served" served
  [[ $(busctl --user call org.andromeda.Lock1 /org/andromeda/Lock1 org.andromeda.Lock1 Lock s user) == "b true" ]] \
    || fail "P1: Lock refused"
  wait_for "P1: the session did not lock" reached "session is locked" 1
  wait_for "P1: face verify did not start" verifying 1
  gaze Finish sssb verify-match usable usable true
  wait_for "P1: a face match did not unlock" reached "session unlocked" 1
  echo "P1 pass: Lock1 locks, a face match unlocks"

  busctl --user call org.andromeda.Lock1 /org/andromeda/Lock1 org.andromeda.Lock1 Lock s user > /dev/null
  wait_for "P2: the session did not lock" reached "session is locked" 2
  # The crash hits while the old process holds the claim, so the new one gets it only after gazed drops the dead owner.
  wait_for "P2: face verify did not start before the crash" verifying 2
  kill -SEGV "$lockpid"; wait "$lockpid" 2> /dev/null || true
  wait_for "P2: the compositor did not keep the lock" reached "destroyed while locked" 1
  start=$(ms)
  start_lock 2
  wait_for "P2: the new lock process did not lock again" reached "session is locked" 3
  took=$(( $(ms) - start ))
  echo "P2: new lock process start -> locked: ${took} ms"
  (( took < 1000 )) || fail "P2: relock took ${took} ms"
  wait_for "P2: face verify did not start" verifying 3
  gaze Finish sssb verify-match usable usable true
  wait_for "P2: a face match did not unlock" reached "session unlocked" 2
  echo "P2 pass: a crashed lock process is replaced and the session stays locked until a match"
  echo PASS
'
