#!/usr/bin/env bash
# Desktop Layouts board parts that no other script shoots: the Settings › Windows gallery in light and dark and with
# the Custom card (board .agent/mockups/desktop-layouts.html section 6), the dock snap-group hover card (board
# desktop-menus.html B), and a macOS title bar and the Windows taskbar row at output scale 1.5. Shots only; the boards
# are the reference. Usage: tests/e2e/desktop_layouts_shots.sh; images in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/desktop-layouts-shots}
source "$(dirname "$0")/lib.sh"
UNMAP=$ROOT/compositor/build-debug/tests/unmap-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

boot_headless 1
# The Andromeda preset's compositor parts, so the gallery reads Andromeda until the dock change makes it Custom.
cat >> "$RUNTIME/andromeda.toml" <<'CONFIG'

[layout]
mode = "scrolling"
[overview]
style = "spaces"
[appearance.title_bar]
mode = "auto"
style = "andromeda"
button_position = "right"
CONFIG
run "$ANDROMEDA" msg config-reload > /dev/null
cat >> "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'CONFIG'

[dock]
enabled = true
auto_hide = false
smart_auto_hide = false
pinned = []
# The Windows status cell; the islands bar shows it only once section 4 lists "group:status".
[[bar.default.capsule_group]]
id = "status"
members = ["status_network", "status_volume", "status_battery"]
fill = "#00000000"
radius = 8
hover_as_one = false
CONFIG

export UNMAP POINTER
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
  window() { "$ANDROMEDA" windows --json | jq -c ".[] | select(.title == \"$1\")"; }
  settings_up() { [[ -n $("$ANDROMEDA" windows --json | jq -r ".[] | select(.app_id == \"dev.noctalia.Noctalia\") | .id") ]]; }

  # 1-2. Gallery: the first card of the Windows page.
  "$NOCTALIA" msg settings-open layout > /dev/null
  for _ in $(seq 100); do settings_up && break; sleep 0.05; done
  settings_up || fail "the Settings window did not map"
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/gallery-light.png"
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  stable_grim "$OUT/gallery-dark.png"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # The fifth (Custom) card sits below the fold: scroll the content column, right of the sidebar.
  read -r wx wy ww wh < <("$ANDROMEDA" windows --json | jq -r ".[] | select(.app_id == \"dev.noctalia.Noctalia\") | \"\(.x) \(.y) \(.w) \(.h)\"")
  "$POINTER" 1280 720 move $((wx + ww * 7 / 10)) $((wy + wh / 2)) axis vertical 240 axis-stop vertical > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/gallery-custom.png"
  "$NOCTALIA" msg config-set dock.enabled true > /dev/null
  "$NOCTALIA" msg settings-close > /dev/null
  for _ in $(seq 100); do settings_up || break; sleep 0.05; done

  # 3. Dock snap group, as compositor/tests/harness/checks/layout/snap_assist.sh: Free mode before the windows map,
  # Snap Layouts + Enter puts desk-a left, then Enter in snap assist puts desk-b right and forms the group.
  "$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
  APP_ID=desk-b FILL_COLOR=0xFF30A060 "$UNMAP" desk-b 600 400 > "$OUT/client-b.log" 2>&1 &
  for _ in $(seq 100); do [[ -n $(window desk-b) ]] && break; sleep 0.05; done
  APP_ID=desk-a FILL_COLOR=0xFF3060C0 "$UNMAP" desk-a 600 400 > "$OUT/client-a.log" 2>&1 &
  for _ in $(seq 100); do [[ -n $(window desk-a) && -n $(window desk-b) ]] && break; sleep 0.05; done
  [[ -n $(window desk-a) && -n $(window desk-b) ]] || fail "the windows did not map"
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg "window-focus:$(window desk-a | jq -r .id)" > /dev/null
  "$ANDROMEDA" msg window-snap-layouts > /dev/null
  "$POINTER" 1280 720 tap 28 > /dev/null
  for _ in $(seq 60); do [[ $(window desk-a | jq -r .snap) == 1.1 ]] && break; sleep 0.05; done
  [[ $(window desk-a | jq -r .snap) == 1.1 ]] || fail "Snap Layouts did not put desk-a in the left half"
  # Snap assist opens once the snap motion ends; the harness runs with animations off.
  "$ANDROMEDA" settle > /dev/null
  "$POINTER" 1280 720 tap 28 > /dev/null
  for _ in $(seq 60); do [[ $(window desk-b | jq -r .snap) == 1.2 ]] && break; sleep 0.05; done
  group=$(window desk-a | jq -r .snap_group)
  [[ $group =~ ^[0-9]+$ && $(window desk-b | jq -r .snap_group) == "$group" ]] || fail "desk-a and desk-b formed no snap group (got $group)"
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  dock=$("$ANDROMEDA" layers --json | jq -c "[.[] | select(.namespace == \"noctalia-dock\" and .mapped)][0] // empty")
  [[ -n $dock ]] || fail "the dock did not map"
  read -r dx dy dw dh < <(jq -r "\"\(.x) \(.y) \(.w) \(.h)\"" <<< "$dock")
  # The icon order (launcher, desk-a, desk-b) is not exposed: rest on each 56 px slot (48 icon + 8 gap) in turn,
  # past the 400 ms preview delay; the slot on a desk icon shows the group card.
  i=0
  for (( x = dx + 32; x <= dx + dw - 32; x += 56 )); do
    i=$((i + 1))
    "$POINTER" 1280 720 move "$x" $((dy + dh / 2)) > /dev/null
    sleep 0.9
    "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
    grim "$OUT/dock-group-$i.png"
  done
  "$POINTER" 1280 720 move 640 300 > /dev/null

  # 4. Scale 1.5: a macOS title bar on desk-a and the Windows taskbar row.
  printf "\n[output.HEADLESS-1]\nscale = 1.5\n" >> "$RUNTIME/andromeda.toml"
  sed -i "s/^mode = \"auto\"/mode = \"forced\"/; s/^style = \"andromeda\"/style = \"macos\"/; s/^button_position = .*/button_position = \"left\"/" "$RUNTIME/andromeda.toml"
  "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do [[ $("$ANDROMEDA" outputs --json | jq -r ".[0].scale") == 1.5 ]] && break; sleep 0.05; done
  [[ $("$ANDROMEDA" outputs --json | jq -r ".[0].scale") == 1.5 ]] || fail "the output did not take scale 1.5"
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null
  "$NOCTALIA" msg config-set bar.default.position "\"bottom\"" > /dev/null
  "$NOCTALIA" msg config-set bar.default.islands false > /dev/null
  "$NOCTALIA" msg config-set bar.default.thickness 48 > /dev/null
  "$NOCTALIA" msg config-set bar.default.margin_ends 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.margin_edge 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.padding 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.widget_spacing 4 > /dev/null
  "$NOCTALIA" msg config-set bar.default.radius_top_left 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.radius_top_right 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.radius_bottom_left 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.radius_bottom_right 0 > /dev/null
  "$NOCTALIA" msg config-set bar.default.start "[]" > /dev/null
  "$NOCTALIA" msg config-set bar.default.center "[\"start\", \"windows_taskbar\"]" > /dev/null
  "$NOCTALIA" msg config-set bar.default.end "[\"tray\", \"group:status\", \"windows_clock\", \"notifications\", \"show_desktop\"]" > /dev/null
  "$ANDROMEDA" msg "window-focus:$(window desk-a | jq -r .id)" > /dev/null
  read -r lw lh < <("$ANDROMEDA" outputs --json | jq -r ".[0].logical_size | \"\(.width) \(.height)\"")
  for _ in $(seq 100); do
    [[ $("$ANDROMEDA" outputs --json | jq -r ".[0].usable_area.height") -lt $lh ]] && break
    sleep 0.1
  done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/scale15-title.png"
  grim -g "0,$((lh - 48)) ${lw}x48" "$OUT/scale15-taskbar.png"
'
ls "$OUT"/*.png
