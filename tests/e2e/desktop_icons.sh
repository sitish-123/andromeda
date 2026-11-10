#!/usr/bin/env bash
# Desktop icons (DL3, mockup .agent/mockups/desktop-icons.html): ~/Desktop on the wallpaper, column from the top-right
# (macOS) corner, a click selects (plate + name pill), a drag on empty space selects by rubber band, and Delete moves the
# selection to Trash. Shots for the board compare in $OUT; the Trash step is the assertion.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/desktop-icons}
source "$(dirname "$0")/lib.sh"
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
K_DELETE=111

boot_headless 1
DESK=$RUNTIME/home/Desktop
mkdir -p "$DESK/Projects" "$DESK/Kathmandu trip"
printf '%%PDF-1.4\n' > "$DESK/Quarterly report final version 2026.pdf"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$DESK/Wallpaper photo.png"
printf 'notes\n' > "$DESK/notes.txt"
cat >> "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'CONFIG'

[desktop_icons]
enabled = true
start_corner = "top_right"
CONFIG

with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  # The headless seat has a keyboard only while a virtual keyboard exists, and the shell binds wl_keyboard when it
  # appears: a key sent in the same burst as its click was lost while the shell bound it (log: "keyboard: bound" and
  # "released" around the press, no key). One keyboard held for the whole test keeps it bound.
  tail -f /dev/null | "'"$POINTER"'" 1280 720 keyboard-only mod none hold > /dev/null &
  keyboard_bound() { [[ $(grep -c "keyboard: bound" "$OUT/noctalia.log") -gt $(grep -c "keyboard: released" "$OUT/noctalia.log") ]]; }
  for _ in $(seq 100); do keyboard_bound && break; sleep 0.05; done
  keyboard_bound || { echo "FAIL: the held keyboard did not bind"; exit 1; }
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/rest.png"
  # Icons: folders first, top-right column from the usable area; the second cell is "Projects" after "Kathmandu trip".
  area=$("$ANDROMEDA" outputs --json | jq -c ".[0].usable_area")
  right=$(( $(jq -r ".x + .width" <<< "$area") ))
  top=$(jq -r .y <<< "$area")
  x=$(( right - 16 - 48 ))
  y=$(( top + 16 + 104 + 30 ))
  "'"$POINTER"'" 1280 720 move "$x" "$y" click 272 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/selected.png"
  # Context menus: on the icon, then on empty desktop (Sort By, Clean Up, Snap to Grid).
  "'"$POINTER"'" 1280 720 move "$x" "$y" click 273 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/icon-menu.png"
  "'"$POINTER"'" 1280 720 tap 1 > /dev/null # Escape
  "'"$POINTER"'" 1280 720 move $(( right - 400 )) $(( top + 300 )) click 273 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/desktop-menu.png"
  "'"$POINTER"'" 1280 720 tap 1 > /dev/null
  # Rubber band from empty space over the first two cells.
  "'"$POINTER"'" 1280 720 move $(( right - 230 )) $(( top + 8 )) press 272 move $(( right - 80 )) $(( top + 80 )) move $(( right - 4 )) $(( top + 200 )) > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/band.png"
  "'"$POINTER"'" 1280 720 move-release $(( right - 4 )) $(( top + 200 )) 272 > /dev/null
  # Delete moves the selection to Trash.
  "'"$POINTER"'" 1280 720 move "$x" "$y" click 272 tap '"$K_DELETE"' > /dev/null
  for _ in $(seq 40); do [[ ! -e "'"$DESK"'/Projects" ]] && break; sleep 0.05; done
  [[ ! -e "'"$DESK"'/Projects" ]] || fail "Delete left Projects on the desktop"
  ls "'"$RUNTIME"'/home/.local/share/Trash/files" | grep -q "^Projects$" || fail "Projects is not in Trash"
  grim "$OUT/after-trash.png"
  # F2 renames in place: the name without its extension starts selected, so typing "x" gives x.txt. With Projects gone,
  # notes.txt moved up into the second cell.
  "'"$POINTER"'" 1280 720 move "$x" "$y" click 272 tap 60 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/rename.png"
  "'"$POINTER"'" 1280 720 tap 45 tap 28 > /dev/null
  for _ in $(seq 40); do [[ -e "'"$DESK"'/x.txt" ]] && break; sleep 0.05; done
  [[ -e "'"$DESK"'/x.txt" ]] || fail "F2 + x + Enter did not rename notes.txt to x.txt"
  echo "PASS: desktop icons select, band-select, move to Trash and rename"
'
ls "$OUT"/*.png
