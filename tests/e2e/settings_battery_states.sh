#!/usr/bin/env bash
# Settings › Battery and Power edge states (TASK-battery step 6): a battery with no UPower history, then no battery
# at all. Writes battery-{nohistory,none}-{1,2}.png (top, then scrolled) to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-battery-states}
source "$(dirname "$0")/lib.sh"
boot_headless 1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
MOCK="$ROOT/tests/e2e/mock_power.py"
SYSTEM_BIN=${ANDROMEDA_SYSTEM:-$ROOT/system/target/debug/andromeda-system}
with_noctalia '
  kill %1; wait %1 2> /dev/null || true
  dbus-daemon --session --nofork --print-address=3 3> "$OUT/system-bus" &
  for _ in $(seq 50); do grep -q unix: "$OUT/system-bus" && break; sleep 0.1; done
  export DBUS_SYSTEM_BUS_ADDRESS=$(head -n1 "$OUT/system-bus")
  # The shell reads the battery from andromeda-system, which reads the mock UPower.
  system_part() {
    ANDROMEDA_SYSTEM_TEST=1 STATE_DIRECTORY="$OUT" '"$SYSTEM_BIN"' > "$OUT/system-$1.log" 2>&1 &
    system=$!
    for _ in $(seq 50); do busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status org.andromeda.System1 > /dev/null 2>&1 && break; sleep 0.1; done
  }
  shot() {
    "$NOCTALIA" > "$OUT/noctalia-$1.log" 2>&1 &
    shell=$!
    for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
    "$NOCTALIA" msg settings-open power > /dev/null
    sleep 2
    grim "$OUT/battery-$1-1.png"
    "$POINTER" 1280 720 move 760 400 $(printf "notch 1 %.0s" $(seq 8)) > /dev/null
    sleep 1
    grim "$OUT/battery-$1-2.png"
    kill "$shell"; wait "$shell" 2> /dev/null || true
  }
  MOCK_BATTERY=1 MOCK_HISTORY=0 python3 '"$MOCK"' "$OUT/profiles.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  system_part nohistory
  shot nohistory
  kill "$system" "$mock"; wait "$system" "$mock" 2> /dev/null || true
  : > "$OUT/mock.log"
  python3 '"$MOCK"' "$OUT/profiles.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  system_part none
  shot none
  kill "$system" "$mock"
'
echo "PASS: artifacts in $OUT"
