#!/usr/bin/env bash
# Desktop Layouts, Windows: the taskbar row's Show Desktop strip at the far end minimizes the workspace's windows on a
# click and brings them back on the next one (DESIGN.md "Desktop Layouts"; .agent/TASK-overview.md DL2, DL4).
# Fails when the show_desktop widget is missing from the bar, does not take the click, or the shell has no
# show-desktop command.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/taskbar-show-desktop}
source "$(dirname "$0")/lib.sh"
UNMAP=$ROOT/compositor/build-debug/tests/unmap-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

boot_headless 1
printf '\n[layout]\nmode = "floating"\n' >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null
cat >> "$RUNTIME/home/.config/noctalia/config.toml" <<'CONFIG'

[bar.default]
position = "bottom"
islands = false
start = []
center = ["start", "taskbar"]
thickness = 48
margin_ends = 0
margin_edge = 0
padding = 0
radius_top_left = 0
radius_top_right = 0
radius_bottom_left = 0
radius_bottom_right = 0
end = ["tray", "clock", "show_desktop"]

[dock]
enabled = false
CONFIG

with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  minimized() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"desk-a\") | .minimized"; }
  wait_minimized() { for _ in $(seq 60); do [[ $(minimized) == "$1" ]] && return 0; sleep 0.05; done; return 1; }
  FILL_COLOR=0xFF3060C0 "'"$UNMAP"'" desk-a 600 400 > "$OUT/client.log" 2>&1 &
  for _ in $(seq 100); do [[ $(minimized) == false ]] && break; sleep 0.05; done
  [[ $(minimized) == false ]] || fail "the window did not map"
  "$ANDROMEDA" settle > /dev/null
  # Wait for the bar to reserve its row, then click the 8 px strip at the far end of the flush row.
  for _ in $(seq 100); do
    area=$("$ANDROMEDA" outputs --json | jq -r ".[0].usable_area.height // empty" 2> /dev/null || true)
    [[ -n $area && $area -lt 720 ]] && break
    sleep 0.1
  done
  [[ -n $area && $area -lt 720 ]] || fail "the bottom bar reserved no row"
  y=$(( (720 + area) / 2 ))
  grim "$OUT/before.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized true || fail "the Show Desktop strip did not minimize the window"
  grim "$OUT/shown.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized false || fail "a second click did not bring the window back"
  echo "PASS: Show Desktop minimized the window and brought it back"
'
