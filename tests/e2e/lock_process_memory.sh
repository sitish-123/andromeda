#!/usr/bin/env bash
# Memory of the resident lock process (`noctalia lock`, .agent/TASK-lock-process.md G4) while it waits unlocked.
# Reads /proc/<pid>/smaps_rollup at start, locked, after unlock and after unlock + 5 s for 3 lock/unlock cycles, and
# prints the warm lock time (Lock1 Lock -> compositor "session is locked") per cycle.
# Fails when Private_Dirty after the last unlock + 5 s exceeds BUDGET_KB (default 34816: 17.5 MB base + ~14 MB GL programs and text libraries kept for a fast warm lock).
# Writes lock.log, smaps-*.txt, locked-*.png, andromeda.log, mock.log and calls.txt to $OUT
# (default ./artifacts/lock-process-memory).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-process-memory}
BUDGET_KB=${BUDGET_KB:-34816}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
printf '\n[lockscreen]\nface = true\nface_on_request = false\n' >> "$RUNTIME/home/.config/noctalia/config.toml"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
BUDGET_KB=$BUDGET_KB with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  count() { grep -c "$1" "$OUT/andromeda.log" || true; }
  reached() { [[ $(count "$1") -ge $2 ]]; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze "$@" \
    > /dev/null; }
  verifying() { [[ $(grep -c "^VerifyStartFor" "$OUT/calls.txt" 2> /dev/null || true) -ge $1 ]]; }
  served() { busctl --user status org.andromeda.Lock1 > /dev/null 2>&1; }
  kb() { awk -v k="$1:" "\$1 == k { print \$2 }" /proc/$lockpid/smaps_rollup; }
  sample() {
    cp /proc/$lockpid/smaps "$OUT/smaps-$1.txt"
    last_dirty=$(kb Private_Dirty)
    printf "%-16s Pss %6d kB  Private_Dirty %6d kB  Anonymous %6d kB\n" "$1" "$(kb Pss)" "$last_dirty" "$(kb Anonymous)"
  }
  # HH:MM:SS.mmm at the start of the nth line that matches $2 in $1, as ms of the day.
  stamp() {
    grep -a "$2" "$1" | sed -n "$3p" | sed -E "s/^([0-9]+):([0-9]+):([0-9]+)\.([0-9]+).*/\1 \2 \3 \4/" \
      | awk "{ print ((\$1 * 60 + \$2) * 60 + \$3) * 1000 + \$4 }"
  }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze > /dev/null
  # A keyboard must exist before the lock surface maps, or the surface never receives keyboard focus.
  wtype -s 3600000 &
  sleep 0.3

  "$NOCTALIA" lock > "$OUT/lock.log" 2>&1 &
  lockpid=$!
  wait_for "org.andromeda.Lock1 is not served" served
  sleep 1
  sample start
  for cycle in 1 2 3; do
    [[ $(busctl --user call org.andromeda.Lock1 /org/andromeda/Lock1 org.andromeda.Lock1 Lock s user) == "b true" ]] \
      || fail "cycle $cycle: Lock refused"
    wait_for "cycle $cycle: the session did not lock" reached "session is locked" $cycle
    wait_for "cycle $cycle: face verify did not start" verifying $cycle
    # Long enough for the wallpaper decode and upload to finish.
    sleep 2
    sample "locked-$cycle"
    grim "$OUT/locked-$cycle.png" 2> /dev/null || true
    gaze Finish sssb verify-match usable usable true
    wait_for "cycle $cycle: a face match did not unlock" reached "session unlocked" $cycle
    sleep 0.3
    sample "unlocked-$cycle"
    sleep 5
    sample "unlocked-$cycle+5s"
    requested=$(stamp "$OUT/lock.log" "lock requested (user)" $cycle)
    locked=$(stamp "$OUT/andromeda.log" "session is locked" $cycle)
    echo "cycle $cycle: lock request -> locked $(( locked - requested )) ms"
  done
  (( last_dirty <= BUDGET_KB )) || fail "unlocked Private_Dirty ${last_dirty} kB > ${BUDGET_KB} kB"
  echo PASS
'
