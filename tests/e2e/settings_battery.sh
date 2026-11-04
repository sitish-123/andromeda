#!/usr/bin/env bash
# Settings › Battery and Power against mockup A (.agent/mockups/battery.html): a mock battery with 24 h of history,
# a day of screen activity, andromeda-system's system part on a private system bus and its session part on the
# session bus. Low battery starts power.low_power_plan, so "Limited by" and "What the desktop did" show. Writes
# light/dark screenshots of the top, the middle and the end of the page to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-battery}
source "$(dirname "$0")/lib.sh"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
MOCK="$ROOT/tests/e2e/mock_power.py"
SYSTEM_BIN=${ANDROMEDA_SYSTEM:-$ROOT/system/target/debug/andromeda-system}
STATE="$RUNTIME/home/.local/state/andromeda"
export STATE
mkdir -p "$STATE"
now=$(date +%s)
for h in $(seq 23 -1 0); do
  start=$(((now / 3600 - h) * 3600))
  hour=$(((start / 3600) % 24))
  if ((hour >= 8 && hour < 23)); then echo "$start $((1200 + (hour * 97) % 1800)) $((120 + (hour * 53) % 480))"; fi
  # Exact spans for screen-on time: in use, idle, off while awake; asleep at night.
  if ((h == 0)); then continue; fi # the running hour: the compositor records it
  if ((hour >= 8 && hour < 23)); then
    echo "s $start $((start + 2400)) n"; echo "s $((start + 2400)) $((start + 3000)) i"; echo "s $((start + 3000)) $((start + 3600)) o"
  else
    echo "s $start $((start + 3600)) z"
  fi
done > "$STATE/screen-activity"
# Per hour: app CPU s, all CPU s, package J. Firefox 5 Wh in the last day; Files (2 days ago, closed) 2.5 Wh, Spotify 1.25 Wh.
hr=$((now / 3600))
{
  echo "d $((hr - 1)) 1800 app-gnome-org.mozilla.firefox-1234.scope"; echo "t $((hr - 1)) 3600"; echo "e $((hr - 1)) 36000"
  echo "d $((hr - 50)) 900 app-gnome-org.gnome.Nautilus-55.scope"; echo "t $((hr - 50)) 1800"; echo "e $((hr - 50)) 18000"
  echo "d $((hr - 75)) 450 flatpak-com.spotify.Client-9.scope"; echo "t $((hr - 75)) 900"; echo "e $((hr - 75)) 9000"
} > "$STATE/app-energy"
# After the fixture: the compositor records into the same file and keeps what it read at start.
boot_headless 1
# Explain logs and rule settings of earlier runs would show as old entries.
rm -rf "$OUT/session-state" "$OUT/session-config" "$OUT/explain.jsonl"
: > "$OUT/profiles.txt"
with_noctalia '
  kill %1; wait %1 2> /dev/null || true
  dbus-daemon --session --nofork --print-address=3 3> "$OUT/system-bus" &
  sysbus=$!
  for _ in $(seq 50); do grep -q unix: "$OUT/system-bus" && break; sleep 0.1; done
  export DBUS_SYSTEM_BUS_ADDRESS=$(head -n1 "$OUT/system-bus")
  MOCK_BATTERY=1 MOCK_UNPLUGGED_HOURS=6 python3 '"$MOCK"' "$OUT/profiles.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  RUST_LOG=debug ANDROMEDA_SYSTEM_TEST=1 STATE_DIRECTORY="$OUT" '"$SYSTEM_BIN"' > "$OUT/system.log" 2>&1 &
  system=$!
  for _ in $(seq 50); do busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status org.andromeda.System1 > /dev/null 2>&1 && break; sleep 0.1; done
  RUST_LOG=debug XDG_STATE_HOME="$OUT/session-state" XDG_CONFIG_HOME="$OUT/session-config" ANDROMEDA_RULES_DIR='"$ROOT"'/system/data/rules \
    '"$SYSTEM_BIN"' --session > "$OUT/session.log" 2>&1 &
  session=$!
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  shell=$!
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  power() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call org.freedesktop.UPower /org/freedesktop/UPower dsk.test.Power "$@" > /dev/null; }
  power SetBattery du 18 2
  power SetOnBattery b true
  for _ in $(seq 50); do grep -q "Hold=power-saver" "$OUT/profiles.txt" && break; sleep 0.1; done
  if ! grep -q "Hold=power-saver" "$OUT/profiles.txt"; then
    busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" introspect org.andromeda.System1 /org/andromeda/System1/Battery/DisplayDevice > "$OUT/battery.txt" 2>&1 || true
    busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" introspect org.andromeda.System1 /org/andromeda/System1/Power > "$OUT/power.txt" 2>&1 || true
    busctl --user tree org.andromeda.System1 > "$OUT/session-tree.txt" 2>&1 || true
    echo "FAIL: low_power_plan did not hold power-saver"; exit 1
  fi
  scroll() { "$POINTER" 1280 720 move 760 400 $(printf "notch 1 %.0s" $(seq "$1")) > /dev/null; sleep 1; }
  cp "$STATE/screen-activity" "$OUT/screen-activity"
  tr "\\0" "\\n" < /proc/$shell/environ | grep -E "^(HOME|XDG_)" > "$OUT/shell-env.txt" || true
  for mode in light dark; do
    "$NOCTALIA" msg theme-mode-set "$mode" > /dev/null
    "$NOCTALIA" msg settings-open power > /dev/null
    sleep 2
    grim "$OUT/battery-$mode-1.png"
    scroll 8
    grim "$OUT/battery-$mode-2.png"
    scroll 30
    grim "$OUT/battery-$mode-3.png"
    "$NOCTALIA" msg settings-close > /dev/null
  done
  # The 10 days range (dark): Last 10 days sits in the title row.
  "$NOCTALIA" msg settings-open power > /dev/null
  sleep 2
  "$POINTER" 1280 720 move 1032 113 click 272 > /dev/null
  sleep 1
  grim "$OUT/battery-10days-1.png"
  scroll 8
  grim "$OUT/battery-10days-2.png"
  cp "$STATE/app-energy" "$OUT/app-energy"
  kill "$shell" "$session" "$system" "$mock" "$sysbus"
'
echo "PASS: artifacts in $OUT"
