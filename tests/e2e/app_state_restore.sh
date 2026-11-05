#!/usr/bin/env bash
# libdesk-app P1: an app restores its window after a kill (docs/apps.md §8).
# desk-app-sample runs in a floating layout. Fails when:
#   - a corner drag in the app's resize band does not resize the window or does not reach its state.json,
#   - after `kill -9` and a relaunch the window does not come back at the dragged size,
#   - the header maximize button does not maximize, or the relaunch after a kill is not maximized
#     (with general.honor_restored_maximize on; the compositor ignores a restored maximize by default).
# Writes windows-*.json, state.json and the app logs to $OUT (default ./artifacts/app-state-restore).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/app-state-restore}
source "$(dirname "$0")/lib.sh"
SAMPLE=${SAMPLE:-$ROOT/libdesk-app/build/desk-app-sample}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.AppSample
STATE=$RUNTIME/home/.local/state/andromeda/$APP/state.json
boot_headless 1
printf '\n[layout]\nmode = "floating"\n' >> "$RUNTIME/andromeda.toml"
# Restored maximize is the compositor's policy (general.honor_restored_maximize, off by default): the test turns it
# on to check that the app asks for it.
sed -i 's/^\[general\]$/[general]\nhonor_restored_maximize = true/' "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null

fail() { echo "FAIL: $*"; exit 1; }
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
start_app() {
  run env -u DBUS_SESSION_BUS_ADDRESS NOCTALIA_LOG_LEVEL=debug XDG_STATE_HOME="$RUNTIME/home/.local/state" "${@:2}" \
    "$SAMPLE" > "$OUT/app-$1.log" 2>&1 &
  app_pid=$!
  for _ in $(seq 100); do
    w=$(win)
    # Input waits for the open animation: settle returns once no animation is pending.
    [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && run "$ANDROMEDA" settle > /dev/null && return 0
    sleep 0.05
  done
  fail "the sample window did not map ($1)"
}
wait_state() { # jq filter on state.json
  for _ in $(seq 60); do [[ -f $STATE ]] && jq -e "$1" "$STATE" > /dev/null 2>&1 && return 0; sleep 0.05; done
  return 1
}
# $app_pid is the `run` subshell; the compositor knows the app's own pid.
kill_app() {
  local pid
  pid=$(win | jq -r .pid)
  [[ $pid =~ ^[0-9]+$ ]] || fail "no pid for the sample window"
  kill -9 "$pid"
  wait "$app_pid" 2> /dev/null || true
  for _ in $(seq 50); do [[ -z $(win) ]] && return; sleep 0.05; done
  fail "the killed app's window is still mapped"
}

start_app first
rm -f "$OUT/state.json"
w=$(win); echo "$w" > "$OUT/windows-first.json"
read -r x y width height < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$w")
# Drag the bottom-right corner 120 x 80 outward from inside its 16 px corner zone (the app's resize band).
run "$POINTER" 1280 720 move $((x + width - 4)) $((y + height - 4)) press 272 \
  move $((x + width + 56)) $((y + height + 36)) move $((x + width + 116)) $((y + height + 76)) release 272 > /dev/null
# The drag has ended when the pointer client returns; wait until the window and state.json agree on one size.
saved=false
for _ in $(seq 60); do
  w=$(win)
  read -r dragged_w dragged_h < <(jq -r '"\(.w) \(.h)"' <<< "$w")
  if (( dragged_w > width )) && [[ -f $STATE ]] \
    && jq -e ".window.main.width == $dragged_w and .window.main.height == $dragged_h" "$STATE" > /dev/null 2>&1; then
    saved=true && break
  fi
  sleep 0.05
done
cp "$STATE" "$OUT/state.json" 2> /dev/null || true
(( dragged_w > width && dragged_h > height )) || fail "the corner drag did not resize ($width x $height -> $dragged_w x $dragged_h)"
$saved || { fail "state.json does not hold $dragged_w x $dragged_h"; }
kill_app

start_app second
w=$(win); echo "$w" > "$OUT/windows-second.json"
read -r width2 height2 < <(jq -r '"\(.w) \(.h)"' <<< "$w")
[[ $width2 == "$dragged_w" && $height2 == "$dragged_h" ]] \
  || fail "relaunch after kill -9 opened at $width2 x $height2, not $dragged_w x $dragged_h"
echo "PASS size restored after kill -9 ($width2 x $height2)"

# The maximize button: second of the three 24 px circles, 12 from the right edge, gap 8, centred in the 48 header.
read -r x y width < <(jq -r '"\(.x) \(.y) \(.w)"' <<< "$w")
run "$POINTER" 1280 720 move $((x + width - 12 - 24 - 8 - 12)) $((y + 24)) click 272 > /dev/null
for _ in $(seq 60); do [[ $(win | jq .maximized) == true ]] && break; sleep 0.05; done
[[ $(win | jq .maximized) == true ]] || fail "the maximize button did not maximize"
wait_state '.window.main.maximized == true' || fail "state.json does not record maximized"
kill_app

start_app third
w=$(win); echo "$w" > "$OUT/windows-third.json"
[[ $(jq .maximized <<< "$w") == true ]] || fail "relaunch after kill -9 is not maximized"
cp "$STATE" "$OUT/state.json"
jq -e ".window.main.width == $dragged_w" "$STATE" > /dev/null || fail "maximizing overwrote the saved floating size"
echo "PASS maximized restored after kill -9; floating size kept"
kill "$(win | jq -r .pid)" 2> /dev/null || true
