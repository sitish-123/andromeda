#!/usr/bin/env bash
# Desktop Layouts, macOS (board .agent/mockups/desktop-layouts.html section 7): the full-width 32 px top row with the
# brand menu, the focused app's name, the status cell and the clock, over a window, light and dark. Shots only; the
# board is the reference. Usage: tests/e2e/macos_layout_shots.sh; images in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/macos-layout}
source "$(dirname "$0")/lib.sh"
UNMAP=$ROOT/compositor/build-debug/tests/unmap-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

boot_headless 1
printf '\n[layout]\nmode = "floating"\n[appearance.title_bar]\nmode = "forced"\nstyle = "macos"\nbutton_position = "left"\n' \
  >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null
cat >> "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'CONFIG'

[bar.default]
position = "top"
islands = false
start = ["brand", "macos_app"]
center = []
end = ["group:status", "macos_clock", "notifications"]
thickness = 32
margin_ends = 0
margin_edge = 0
padding = 12
padding_end = 16
widget_spacing = 16
radius_top_left = 0
radius_top_right = 0
radius_bottom_left = 0
radius_bottom_right = 0
[[bar.default.capsule_group]]
id = "status"
members = ["status_network", "status_volume", "status_battery"]
fill = "#00000000"
radius = 8
hover_as_one = false

[dock]
enabled = true

[desktop_icons]
enabled = true
start_corner = "top_right"
CONFIG
mkdir -p "$RUNTIME/home/Desktop/Projects"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$RUNTIME/home/Desktop/Wallpaper photo.png"

with_noctalia '
  # The theme change fades; a shot is taken once two grabs 0.2 s apart are identical.
  stable_grim() {
    for _ in $(seq 25); do
      grim "$1.prev"; sleep 0.2; grim "$1"
      cmp -s "$1.prev" "$1" && { rm -f "$1.prev"; return 0; }
    done
    echo "FAIL: the screen did not settle for $1"; exit 1
  }
  APP_ID=macos-a FILL_COLOR=0xFFF2F2F6 "'"$UNMAP"'" macos-a 640 400 > "$OUT/client.log" 2>&1 &
  for _ in $(seq 100); do
    [[ $("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"macos-a\") | .title") == macos-a ]] && break
    sleep 0.05
  done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/light.png"
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  stable_grim "$OUT/dark.png"
  # The brand mark opens the session menu (Sleep, Restart, Shut Down, Lock).
  "'"$POINTER"'" 1280 720 move 30 16 click 272 > /dev/null
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" layers --json | jq "[.[] | select((.namespace == \"noctalia-panel\" or .namespace == \"noctalia-attached-panel\") and .mapped)] | length") -gt 0 ]] && break
    sleep 0.05
  done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/brand-menu.png"
  "$ANDROMEDA" layers --json > "$OUT/brand-layers.json"
  [[ $("$ANDROMEDA" layers --json | jq "[.[] | select((.namespace == \"noctalia-panel\" or .namespace == \"noctalia-attached-panel\") and .mapped)] | length") -gt 0 ]] \
    || { echo "FAIL: a click on the brand mark opened no menu"; exit 1; }
  "'"$POINTER"'" 1280 720 tap 1 > /dev/null # Escape
  # Window menu fallback (board desktop-menus.html A3): a click on the bold app name, right of the brand mark.
  "'"$POINTER"'" 1280 720 move 64 16 click 272 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/window-menu.png"
  "'"$POINTER"'" 1280 720 tap 1 > /dev/null # Escape
  # The full-page power choices (D3 section 1): Ctrl+Alt+Delete shows them; Right moves the white focus; Esc closes.
  "$NOCTALIA" msg power-overlay > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  stable_grim "$OUT/power-overlay.png"
  # Pause first: the shell binds the per-run virtual keyboard after its first key.
  "'"$POINTER"'" 1280 720 tap 119 pause 100 tap 106 > /dev/null # Right
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/power-overlay-right.png"
  "'"$POINTER"'" 1280 720 tap 119 pause 100 tap 1 > /dev/null # Escape
  for _ in $(seq 40); do
    [[ $("$ANDROMEDA" layers --json | jq "[.[] | select(.namespace == \"noctalia-power-overlay\")] | length") -eq 0 ]] && break
    sleep 0.05
  done
  [[ $("$ANDROMEDA" layers --json | jq "[.[] | select(.namespace == \"noctalia-power-overlay\")] | length") -eq 0 ]] \
    || { echo "FAIL: Escape did not close the power overlay"; exit 1; }
  # An exported menubar (board section 7, light): a GTK 4 window named Files; Right moves from the app name to File.
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  GDK_BACKEND=wayland python3 -I "'"$ROOT"'/tests/e2e/menu_app.py" Files > "$OUT/menu-app.log" 2>&1 &
  for _ in $(seq 200); do
    [[ $("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Files\") | .title") == Files ]] && break
    sleep 0.05
  done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 move 64 16 click 272 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 tap 119 pause 100 tap 106 > /dev/null # Right
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/file-menu.png"
'
ls "$OUT"/*.png
