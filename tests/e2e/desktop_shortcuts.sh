#!/usr/bin/env bash
# Desktop v2 (.agent/TASK-desktop-v2.md): Windows-grade desktop icons in the Andromeda layout.
# Fails when:
#   - desktop_icons.enabled alone (no macOS/Windows layout) does not map the desktop icons,
#   - an app row dragged out of the launcher onto the desktop does not make a shortcut (.desktop copy) there,
#   - the shortcut shows its file name instead of the app name "Drop Catch",
#   - Ctrl+Shift+N does not make "untitled folder" with its name field open, or typing a name and Enter does not
#     rename it,
#   - with Snap to Grid off, a dragged icon does not keep the exact drop point (saved as [col, row, x, y]),
#   - Ctrl+C then Ctrl+V on the desktop does not paste a copy of the folder.
# Shots (launcher drag, desktop menu, item menu) and logs go to $OUT (default ./artifacts/desktop-shortcuts).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/desktop-shortcuts}
source "$(dirname "$0")/lib.sh"
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}

boot_headless 1
printf '\n[layout]\nmode = "floating"\n' >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null
H=$RUNTIME/home
DESK=$H/Desktop
mkdir -p "$DESK" "$H/.local/share/applications"
cat > "$H/.local/share/applications/dropcatch.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Drop Catch
Exec=true
Icon=text-x-generic
DESKTOP
cat >> "$H/.config/noctalia/config.toml" <<'CONFIG'

[desktop_icons]
enabled = true
CONFIG

export POINTER FILEOPS DESK H
export NOCTALIA_LOG_LEVEL=debug
with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  wait_for() { # description, test command...
    local what=$1; shift
    for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
    fail "$what"
  }
  logged() { grep -qsE -- "$2" "$1"; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; }
  tail -f /dev/null | "$POINTER" 1280 720 keyboard-only mod none hold > /dev/null &
  keyboard_bound() { [[ $(grep -c "keyboard: bound" "$OUT/noctalia.log") -gt $(grep -c "keyboard: released" "$OUT/noctalia.log") ]]; }
  wait_for "the held keyboard did not bind" keyboard_bound
  "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
  wait_for "the FileOps1 daemon is not on the bus" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1
  layer() { "$ANDROMEDA" layers --json | jq -c "[.[] | select(.namespace == \"$1\")][0]"; }
  slot_of() { jq -c "[.[]] | map(.[\"$1\"] // empty) | .[0] // empty" "$H/.local/state/andromeda/desktop-icons.json" 2> /dev/null; }

  # 1. Icons in the Andromeda layout: desktop_icons.enabled alone maps the surface.
  wait_for "the desktop did not map without a macOS/Windows layout" bash -c "[[ \$(\"$ANDROMEDA\" layers --json | jq \"[.[] | select(.namespace == \\\"andromeda-desktop-icons\\\" and .w > 0)] | length\") -gt 0 ]]"
  settle
  desk=$(layer andromeda-desktop-icons)
  DX=$(jq .x <<< "$desk"); DY=$(jq .y <<< "$desk")
  echo "PASS icons in the Andromeda layout"

  # 2. Launcher -> desktop: the app row dragged out makes a shortcut where it lands.
  "$NOCTALIA" msg panel-open launcher "Drop Catch" > /dev/null
  wait_for "the launcher did not draw" logged "$OUT/noctalia.log" "open to first frame"
  settle
  panel=$("$ANDROMEDA" layers --json | jq -c "[.[] | select(.namespace | test(\"panel|launcher\"))] | max_by(.w * .h)")
  PX=$(jq .x <<< "$panel"); PY=$(jq .y <<< "$panel"); PW=$(jq .w <<< "$panel"); PH=$(jq .h <<< "$panel")
  "$POINTER" 1280 720 move 2 2 move $((PX + PW / 2)) $((PY + PH / 2)) > /dev/null
  wait_for "the launcher logged no selected row" logged "$OUT/noctalia.log" "launcher selected row Drop Catch at "
  # The log gives the enter point in surface coordinates: the surface origin is the pointer minus it.
  read -r at rx ry rw rh <<< "$(grep -E "launcher selected row Drop Catch at " "$OUT/noctalia.log" | tail -n 1 | awk "{print \$(NF-4), \$(NF-3), \$(NF-2), \$(NF-1), \$NF}")"
  OX=$((PX + PW / 2 - ${at%,*})); OY=$((PY + PH / 2 - ${at#*,}))
  SX=$((OX + ${rx%.*} + ${rw%.*} / 4)); SY=$((OY + ${ry%.*} + ${rh%.*} / 2))
  # Drop well left of the launcher, on empty desktop.
  TX=$((DX + 200)); TY=$((DY + 420))
  "$POINTER" 1280 720 move "$SX" "$SY" pause 300 press 272 move $((SX + 6)) $((SY + 6)) move $((SX - 60)) "$SY" \
    move $((OX - 40)) "$TY" mark out pause 200 move "$TX" "$TY" pause 200 move $((TX + 1)) "$TY" pause 100 release 272 > /dev/null
  wait_for "the launcher did not start an app drag" logged "$OUT/noctalia.log" "launcher app drag started .*dropcatch.desktop"
  wait_for "the drop made no shortcut in ~/Desktop" test -f "$DESK/dropcatch.desktop"
  settle
  grim "$OUT/launcher-drag.png"
  slot=$(slot_of dropcatch.desktop)
  [[ -n $slot ]] || fail "the shortcut got no place under the pointer"
  echo "PASS launcher app dragged to the desktop ($slot)"

  # 3. Ctrl+Shift+N: a new folder, name field open; typing renames it.
  "$POINTER" 1280 720 move $((DX + 600)) $((DY + 600)) click 272 > /dev/null
  "$POINTER" 1280 720 keyboard-only mod control+shift tap 49 mod none > /dev/null
  wait_for "Ctrl+Shift+N made no folder" logged "$OUT/noctalia.log" "desktop new [Uu]ntitled [Ff]older"
  settle
  grim "$OUT/new-folder.png"
  wtype -s 50 Projects -k Return
  wait_for "typing a name did not rename the new folder" test -d "$DESK/Projects"
  echo "PASS new folder with rename"

  # 4. Snap to Grid off: the icon keeps the exact drop point.
  sed -i "s/^enabled = true$/enabled = true\nsnap_to_grid = false/" "$H/.config/noctalia/config.toml"
  wait_for "snap_to_grid = false did not apply" logged "$OUT/noctalia.log" "desktop free placement"
  settle
  # Projects is the first folder: top-left cell (96 x 104, 16 from the edge, plate 72).
  FX=$((DX + 16 + 48)); FY=$((DY + 16 + 36))
  "$POINTER" 1280 720 move "$FX" "$FY" pause 300 press 272 move $((FX + 10)) $((FY + 10)) move $((FX + 150)) $((FY + 130)) \
    move $((FX + 237)) $((FY + 211)) pause 200 move $((FX + 238)) $((FY + 211)) pause 100 release 272 > /dev/null
  wait_for "the free drop saved no exact place" bash -c "[[ \$(jq -c \"[.[]] | map(.[\\\"Projects\\\"] // empty) | .[0] | length\" \"$H/.local/state/andromeda/desktop-icons.json\") == 4 ]]"
  place=$(slot_of Projects)
  x=$(jq ".[2] | floor" <<< "$place"); y=$(jq ".[3] | floor" <<< "$place")
  (( x >= 16 + 238 - 4 && x <= 16 + 238 + 4 && y >= 16 + 211 - 4 && y <= 16 + 211 + 4 )) || fail "the free drop moved off the drop point: $place"
  settle
  grim "$OUT/free-place.png"
  echo "PASS free placement ($place)"

  # 5. Ctrl+C, Ctrl+V: a copy of the folder.
  "$POINTER" 1280 720 move $((FX + 238)) $((FY + 211)) click 272 > /dev/null
  wait_for "the click did not select Projects" bash -c "grep -c \"desktop press Projects\" \"$OUT/noctalia.log\" | grep -qv \"^0$\""
  "$POINTER" 1280 720 keyboard-only mod control tap 46 tap 47 mod none > /dev/null
  wait_for "Ctrl+C then Ctrl+V pasted no copy" bash -c "[[ \$(ls -d \"$DESK\"/Projects* | wc -l) -ge 2 ]]"
  echo "PASS copy and paste"

  # 6. Focus grab: an open panel has no full-screen shield; a click outside closes it and still reaches the desktop.
  "$NOCTALIA" msg panel-open launcher > /dev/null
  wait_for "the launcher did not open again" bash -c "[[ \$(grep -c \"opened \\\"launcher\\\"\" \"$OUT/noctalia.log\") -ge 2 ]]"
  settle
  [[ $(layer noctalia-panel-click-shield) == null ]] || fail "the panel still put a full-screen click shield up"
  # The shortcut sits in cell [1,3] (96 x 104, 16 from the edge), left of the launcher pill.
  SCX=$((DX + 16 + 96 + 48)); SCY=$((DY + 16 + 3 * 104 + 36))
  presses=$(grep -c "desktop press dropcatch.desktop" "$OUT/noctalia.log" || true)
  "$POINTER" 1280 720 move "$SCX" "$SCY" click 272 > /dev/null
  wait_for "a click outside did not close the launcher" bash -c "[[ \$(grep -c \"closing \\\"launcher\\\"\" \"$OUT/noctalia.log\") -ge 2 ]]"
  wait_for "the closing click did not reach the desktop icon" bash -c "[[ \$(grep -c \"desktop press dropcatch.desktop\" \"$OUT/noctalia.log\") -gt $presses ]]"
  echo "PASS outside click closes the panel and passes through"

  # Menus, for the screenshot compare (DESIGN.md "Desktop icons").
  "$POINTER" 1280 720 move $((FX + 238)) $((FY + 211)) click 273 > /dev/null
  settle; grim "$OUT/item-menu.png"
  "$POINTER" 1280 720 keyboard-only tap 1 > /dev/null
  "$POINTER" 1280 720 move $((DX + 700)) $((DY + 300)) click 273 > /dev/null
  settle; grim "$OUT/desktop-menu.png"
  "$POINTER" 1280 720 keyboard-only tap 1 > /dev/null
'
ls "$OUT"/*.png
