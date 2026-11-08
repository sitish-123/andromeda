#!/usr/bin/env bash
# Files v2 FB2 (FILES-UX.md §6 "FB2 views"): Ctrl+1..3 switch icons, list and columns; the columns view walks folders
# with Left/Right and scrolls a new column into view on spring.page (M37) without a page slide (D7).
# Fails when:
#   - Ctrl+4 does not show the Gallery with the folder's photos, Right does not step, Escape does not leave it,
#   - Ctrl+3 does not show the columns view, or Right on a folder does not open it as the next column,
#   - Left does not go back to the parent with the folder selected,
#   - a column change slides the page (files.page motion) instead of scrolling (files.columns motion),
#   - a press in an ancestor column does not move the current folder there,
#   - the sidebar This PC place does not open the This PC page,
#   - Ctrl+T does not open a second tab on the current folder, or Ctrl+Tab / Ctrl+Shift+Tab do not switch tabs,
#   - Right on a folder in the list view does not expand it in place with its child row under it, or Left does not
#     collapse it.
# Shots of each view go to $OUT (default ./artifacts/files-views).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-views}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC/Alpha/Beta/Gamma" "$SRC/Zulu"
for f in a.txt b.pdf; do echo x > "$SRC/$f"; done
echo y > "$SRC/Alpha/Beta/Gamma/deep.md"
python3 -I - "$SRC" <<'PY'
import sys, zlib, struct
def png(path, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * 64 for _ in range(48))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 48, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
png(sys.argv[1] + "/photo-1.png", (81, 71, 229))
png(sys.argv[1] + "/photo-2.png", (229, 120, 40))
PY

printf '#!/bin/sh\npwd > "%s/terminal-cwd"\n' "$OUT" > "$RUNTIME/fake-terminal"
chmod +x "$RUNTIME/fake-terminal"
rm -f "$OUT/terminal-cwd"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug DESK_MOTION_TRACE=1 DESK_FRAME_STATS=1 \
  DESK_FILES_GEOMETRY=1 TERMINAL="$RUNTIME/fake-terminal" "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
WX=$(jq .x <<< "$w")
WY=$(jq .y <<< "$w")
run "$ANDROMEDA" settle > /dev/null

wait_log() {
  for _ in $(seq 60); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
at() {
  local line
  line=$(grep -E "geometry $1( |$)" "$OUT/files.log" | tail -n 1)
  [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"
  echo "$((WX + x)) $((WY + y))"
}

wait_log 0 "location $SRC" || fail "the window did not list $SRC"
shot 1-icons
m=$(mark); keys mod control tap 5 mod none      # Ctrl+4: Gallery
wait_log "$m" "view gallery" || fail "Ctrl+4 did not show the Gallery"
wait_log "$m" "gallery 1 of 2" || fail "the Gallery does not show the 2 photos"
m=$(mark); keys tap 106                          # Right: the next photo
wait_log "$m" "gallery 2 of 2" || fail "Right did not move the Gallery to the second photo"
shot 1b-gallery
m=$(mark); keys tap 1                            # Escape: back to icons
wait_log "$m" "view icons" || fail "Escape did not leave the Gallery"
m=$(mark); keys mod control tap 3 mod none      # Ctrl+2: list
wait_log "$m" "settle files.view " || fail "Ctrl+2 did not switch to the list"
shot 2-list
# FB2 disclosure: Right expands Alpha in place (its child row lands right under it), Left collapses it.
m=$(mark); keys tap 30 tap 38                    # "al": type-ahead to Alpha
wait_log "$m" "cursor Alpha " || fail "type-ahead did not reach Alpha in the list"
m=$(mark); keys tap 106                          # Right: expand
wait_log "$m" "expand Alpha" || fail "Right did not expand Alpha"
wait_log "$m" "expanded rows" || fail "Alpha's items did not join the list"
run "$ANDROMEDA" settle > /dev/null
for _ in $(seq 40); do grep -qE "geometry item Beta " "$OUT/files.log" && break; sleep 0.05; done
alpha_y=$(grep -E "geometry item Alpha " "$OUT/files.log" | tail -n 1 | awk '{print $NF}')
beta_y=$(grep -E "geometry item Beta " "$OUT/files.log" | tail -n 1 | awk '{print $NF}')
[[ -n $beta_y && $((beta_y - alpha_y)) -eq 32 ]] || fail "Beta is not the row under Alpha (Alpha y $alpha_y, Beta y ${beta_y:-none})"
shot 2a-list-expanded
m=$(mark); keys tap 105                          # Left: collapse
wait_log "$m" "collapse Alpha" || fail "Left did not collapse Alpha"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "disclosure Alpha") click 272 > /dev/null   # the triangle expands
wait_log "$m" "expand Alpha" || fail "a press on Alpha's disclosure triangle did not expand it"
tail -n +"$((m + 1))" "$OUT/files.log" | grep -q "cursor Alpha/" && fail "the triangle press selected a row"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "disclosure Alpha") click 272 > /dev/null   # and collapses
wait_log "$m" "collapse Alpha" || fail "a second press on the triangle did not collapse Alpha"
sleep 1.1                                        # the type-ahead buffer resets
m=$(mark); keys mod control tap 4 mod none      # Ctrl+3: columns
wait_log "$m" "view columns" || fail "Ctrl+3 did not show the columns view"

m=$(mark); keys tap 30 tap 38                    # "al": type-ahead to Alpha (the list step left the cursor on it)
wait_log "$m" "cursor Alpha " || fail "type-ahead did not reach Alpha"
m=$(mark); keys tap 106                          # Right: open Alpha as the current column
wait_log "$m" "location $SRC/Alpha\$" || fail "Right did not open Alpha"

# A press in the first column (still in view) moves the current folder back to Work; then Alpha again.
wait_log "$m" "settle " || fail "the columns did not settle after opening Alpha"
shot 2b-columns-alpha
m=$(mark); run "$POINTER" 1280 720 move $(at "column-item Zulu") click 272 > /dev/null
wait_log "$m" "location $SRC\$" || fail "a press in the first column did not move the current folder to Work"
sleep 1.1
m=$(mark); keys tap 30 tap 38 tap 106            # "al", Right: Alpha again (a lone "a" goes to the next match after Zulu)
wait_log "$m" "location $SRC/Alpha\$" || fail "Right did not open Alpha again"
sleep 1.1
m=$(mark); keys tap 48 tap 106                   # b, Right: Beta
wait_log "$m" "location $SRC/Alpha/Beta\$" || fail "Right did not open Beta"
sleep 1.1
keys tap 34 tap 106                              # g, Right: Gamma; 4 columns overflow the stage
wait_log "$m" "location $SRC/Alpha/Beta/Gamma\$" || fail "Right did not open Gamma"
wait_log "$m" "motion files.columns page" || fail "the new column did not scroll in on spring.page"
tail -n +"$((m + 1))" "$OUT/files.log" | grep -q "motion files.page" && fail "the columns view slid the page"
shot 3-columns

m=$(mark); keys tap 105                          # Left: back to Beta with Gamma selected
wait_log "$m" "location $SRC/Alpha/Beta\$" || fail "Left did not go back to Beta"
wait_log "$m" "selected 1 of 1 in $SRC/Alpha/Beta" || fail "Left did not select Gamma in Beta"

grep -E "anim .* missed [1-9]" "$OUT/files.log" && fail "an animation missed frames"

# FB4 tabs: Ctrl+T opens the current folder in a second tab (the strip shows), Ctrl+Tab and Ctrl+Shift+Tab switch.
m=$(mark); keys mod control tap 20 mod none      # Ctrl+T
wait_log "$m" "tab new 2 of 2" || fail "Ctrl+T did not open a second tab"
wait_log "$m" "location $SRC/Alpha/Beta\$" || fail "the new tab did not open the current folder"
shot 3b-tabs
m=$(mark); keys mod control tap 15 mod none      # Ctrl+Tab: wraps to the first tab
wait_log "$m" "tab select 1 of 2" || fail "Ctrl+Tab did not switch to the first tab"
m=$(mark); keys mod control+shift tap 15 mod none # Ctrl+Shift+Tab: back to the second
wait_log "$m" "tab select 2 of 2" || fail "Ctrl+Shift+Tab did not switch back to the second tab"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "tab-add") click 272 > /dev/null            # "+" opens a third tab
wait_log "$m" "tab new 3 of 3" || fail "the + button did not open a third tab"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "tab 0") click 272 > /dev/null              # a press selects a tab
wait_log "$m" "tab select 1 of 3" || fail "a press on the first tab did not select it"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "tab-close 2") click 272 > /dev/null        # its x closes the third
wait_log "$m" "tab close, now [0-9] of 2" || fail "the close button did not close the third tab"

# FB4 details pane (mockup section 2): Alt+P shows it at the right edge; it follows the selection and reads the
# image size from the header.
files_action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "$2" '{}' > /dev/null
}
m=$(mark); files_action go "[<'file://$SRC'>]"
wait_log "$m" "location $SRC\$" || fail "go did not open $SRC"
m=$(mark); keys mod alt tap 25 mod none          # Alt+P (KEY_P)
wait_log "$m" "details pane on" || fail "Alt+P did not show the details pane"
for _ in $(seq 20); do
  m=$(mark); files_action select "[<['photo-1.png']>]"
  wait_log "$m" "details photo-1.png 64x48" && break
done
grep -q "details photo-1.png 64x48" "$OUT/files.log" || fail "the details pane did not show photo-1.png at 64 x 48"
shot 3c-details
m=$(mark); keys mod alt tap 25 mod none
wait_log "$m" "details pane off" || fail "Alt+P did not hide the details pane"

# FB4 Starred (Ctrl+D) and Open Terminal Here (Shift+F4): photo-1.png is still selected.
m=$(mark); keys mod control tap 32 mod none      # Ctrl+D (KEY_D)
wait_log "$m" "starred 1 items" || fail "Ctrl+D did not star photo-1.png"
grep -q "photo-1.png" "$HOME_DIR/.local/share/andromeda/files/starred.json" || fail "starred.json does not hold photo-1.png"
m=$(mark); run "$POINTER" 1280 720 move $(at "place Starred") click 272 > /dev/null
wait_log "$m" "location starred" || fail "the Starred place did not open the Starred listing"
shot 3d-starred
m=$(mark); files_action go "[<'file://$SRC/Alpha'>]"
wait_log "$m" "location $SRC/Alpha\$" || fail "go did not open Alpha"
m=$(mark); keys mod shift tap 62 mod none        # Shift+F4 (KEY_F4)
wait_log "$m" "terminal .*fake-terminal in $SRC/Alpha\$" || fail "Shift+F4 did not open the terminal in Alpha"
for _ in $(seq 40); do [[ -s $OUT/terminal-cwd ]] && break; sleep 0.05; done
[[ $(cat "$OUT/terminal-cwd" 2> /dev/null) == "$SRC/Alpha" ]] || fail "the terminal did not start in Alpha"

# FB4 context menu (mockup §4): a right click on Beta selects it and opens the menu; Down, Enter runs Get Info.
m=$(mark); keys mod control tap 2 mod none       # Ctrl+1: icons
wait_log "$m" "view icons" || fail "Ctrl+1 did not switch to icons"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "item Beta") click 273 > /dev/null
wait_log "$m" "selected 1 of " || fail "the right click did not select Beta"
run "$ANDROMEDA" settle > /dev/null
shot 3e-menu
m=$(mark); keys tap 108 tap 28                   # Down, Enter
wait_log "$m" "menu file.info" || fail "Down + Enter in the menu did not run Get Info"
run "$ANDROMEDA" settle > /dev/null
shot 3f-info
keys tap 1                                       # Esc closes Get Info
run "$ANDROMEDA" settle > /dev/null

# This PC (mockup section 0): the sidebar place shows the folders and drives.
m=$(mark); run "$POINTER" 1280 720 move $(at "place This PC") click 272 > /dev/null
wait_log "$m" "location this-pc" || fail "the This PC place did not open This PC"
shot 4-this-pc
echo "pass files_views"
