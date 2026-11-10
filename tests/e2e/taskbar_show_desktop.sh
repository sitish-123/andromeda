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
cat >> "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'CONFIG'

[bar.default]
position = "bottom"
islands = false
start = []
center = ["start", "windows_taskbar"]
thickness = 48
margin_ends = 0
margin_edge = 0
padding = 0
widget_spacing = 4
radius_top_left = 0
radius_top_right = 0
radius_bottom_left = 0
radius_bottom_right = 0
end = ["tray", "group:status", "windows_clock", "notifications", "show_desktop"]
[[bar.default.capsule_group]]
id = "status"
members = ["status_network", "status_volume", "status_battery"]
fill = "#00000000"
radius = 8
hover_as_one = false

[dock]
enabled = false
CONFIG

with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  # The theme change fades; a shot is taken once two grabs 0.2 s apart are identical.
  stable_grim() {
    local g=()
    [[ $1 == -g ]] && { g=(-g "$2"); shift 2; }
    for _ in $(seq 25); do
      grim "${g[@]}" "$1.prev"; sleep 0.2; grim "${g[@]}" "$1"
      cmp -s "$1.prev" "$1" && { rm -f "$1.prev"; return 0; }
    done
    fail "the screen did not settle for $1"
  }
  minimized() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"desk-a\") | .minimized"; }
  wait_minimized() { for _ in $(seq 60); do [[ $(minimized) == "$1" ]] && return 0; sleep 0.05; done; return 1; }
  APP_ID=desk-a FILL_COLOR=0xFF3060C0 "'"$UNMAP"'" desk-a 600 400 > "$OUT/client.log" 2>&1 &
  for _ in $(seq 100); do [[ $(minimized) == false ]] && break; sleep 0.05; done
  [[ $(minimized) == false ]] || fail "the window did not map"
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # Wait for the bar to reserve its row, then click the 8 px strip at the far end of the flush row.
  for _ in $(seq 100); do
    area=$("$ANDROMEDA" outputs --json | jq -r ".[0].usable_area.height // empty" 2> /dev/null || true)
    [[ -n $area && $area -lt 720 ]] && break
    sleep 0.1
  done
  [[ -n $area && $area -lt 720 ]] || fail "the bottom bar reserved no row"
  y=$(( (720 + area) / 2 ))
  grim "$OUT/before.png"
  row="0,$area 1280x$((720 - area))"
  # Hover states (board section 1). The tray group centre comes from the trailing widths: Show Desktop 8, bell cell
  # 40, clock about 72, 4 apart, then the 80 wide status group.
  "'"$POINTER"'" 1280 720 move 1108 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-tray.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-show-desktop.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized true || fail "the Show Desktop strip did not minimize the window"
  grim "$OUT/shown.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized false || fail "a second click did not bring the window back"
  # Window previews (R6): rest on the app button (right of Start at the row centre) past the 400 ms open delay.
  # The popover has no IPC state, so the shot is the check; it opens on a timer, not an event.
  "'"$POINTER"'" 1280 720 move 651 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview.png"
  # Three windows of one app share one button (combine_apps); the pointer rests on the close button of the middle
  # card (24, 4 in from the card edge, in the 24 header).
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  APP_ID=desk-a FILL_COLOR=0xFFC06030 "'"$UNMAP"'" desk-a2 600 400 > "$OUT/client-a2.log" 2>&1 &
  a2=$!
  APP_ID=desk-a FILL_COLOR=0xFF9040B0 "'"$UNMAP"'" desk-a3 600 400 > "$OUT/client-a3.log" 2>&1 &
  a3=$!
  for _ in $(seq 100); do [[ $("$ANDROMEDA" windows --json | jq length) -ge 3 ]] && break; sleep 0.05; done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 move 651 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # Three cards on the one desk-a button: 632 wide centred on x 651, 180 high, 8 above the 40 cell (4 above the row).
  top=$(( area - 4 - 180 ))
  "'"$POINTER"'" 1280 720 move 651 $(( top + 76 )) move 735 $(( top + 24 )) > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview-3-close.png"
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  kill "$a2" "$a3"
  for _ in $(seq 100); do [[ $("$ANDROMEDA" windows --json | jq length) -eq 1 ]] && break; sleep 0.05; done
  # Snap group (board desktop-menus.html B): Snap Layouts puts desk-a left, snap assist puts desk-b right, and the
  # desk-a preview opens with the group card first. The buttons are now Start, desk-a, desk-b around the row centre.
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  APP_ID=desk-b FILL_COLOR=0xFF30A060 "'"$UNMAP"'" desk-b 600 400 > "$OUT/client-b.log" 2>&1 &
  window() { "$ANDROMEDA" windows --json | jq -c ".[] | select(.title == \"$1\")"; }
  for _ in $(seq 100); do [[ -n $(window desk-b) ]] && break; sleep 0.05; done
  "$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
  "$ANDROMEDA" msg "window-focus:$(window desk-a | jq -r .id)" > /dev/null
  "$ANDROMEDA" msg window-snap-layouts > /dev/null
  "'"$POINTER"'" 1280 720 tap 28 > /dev/null
  for _ in $(seq 60); do [[ $(window desk-a | jq -r .snap) == 1.1 ]] && break; sleep 0.05; done
  [[ $(window desk-a | jq -r .snap) == 1.1 ]] || fail "Snap Layouts did not put desk-a in the left half"
  # Snap assist opens once the snap motion ends (compositor/tests/harness/checks/layout/snap_assist.sh runs with
  # animations off).
  "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 tap 28 > /dev/null
  for _ in $(seq 60); do [[ $(window desk-b | jq -r .snap) == 1.2 ]] && break; sleep 0.05; done
  group=$(window desk-a | jq -r .snap_group)
  [[ $group =~ ^[0-9]+$ && $(window desk-b | jq -r .snap_group) == "$group" ]] || fail "desk-a and desk-b formed no snap group (got $group)"
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 move 640 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview-group.png"
  # Dark taskbar at rest and with the pointer on an app button.
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  stable_grim -g "$row" "$OUT/taskbar-dark.png"
  "'"$POINTER"'" 1280 720 move 640 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-app-dark.png"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  echo "PASS: Show Desktop minimized the window and brought it back"
'
