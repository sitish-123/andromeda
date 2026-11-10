#!/usr/bin/env bash
# Files actions a user takes with keys and the mouse (user, 2026-11-08: "every action test"). Each step drives the
# action as a user does and checks its result. Fails when:
#   - Alt+Left / Alt+Right / Alt+Up / Alt+Home do not go back, forward, up and home,
#   - the mouse back button (BTN_SIDE) does not go back,
#   - Ctrl+L, typing a folder name and Enter do not open that folder,
#   - Ctrl+H does not show a hidden file,
#   - Space does not open Quick Look, or a second Space does not close it,
#   - Alt+Enter does not open Get Info,
#   - a middle click on a folder does not open it in a new tab,
#   - Ctrl+V does not paste the clipboard text into the search field,
#   - a right click on the search field does not open its edit menu,
#   - a middle click on the search field does not paste the primary selection,
#   - a frame callback takes more than 50 ms after the first frame (main-thread stall).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-actions}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
A=$HOME_DIR/alpha
boot_headless 1
mkdir -p "$OUT" "$A/beta/gamma" "$A/delta"
fail() { echo "FAIL: $*"; exit 1; }
echo one > "$A/note.txt"
echo hidden > "$A/.secret"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

LOG=$OUT/files.log
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 ANDROMEDA_LOG_LEVEL=debug \
  DESK_FILES_GEOMETRY=1 "$FILES" "$A" > "$LOG" 2>&1 &
for _ in $(seq 100); do
  w=$(run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty")
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w")
mark() { wc -l < "$LOG"; }
wait_log() { for _ in $(seq 100); do grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$LOG") && return 0; sleep 0.05; done; return 1; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
at() { local line; line=$(grep -E "geometry $1( |$)" "$LOG" | tail -n 1); [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"; echo "$((WX + x)) $((WY + y))"; }
wait_log 0 "location $A\$" || fail "Files did not open $A"
wait_log 0 "geometry item beta" || fail "no geometry for beta"
run "$ANDROMEDA" settle > /dev/null

# Back, forward, up, home (Alt+Left, Alt+Right, Alt+Up, Alt+Home) and the mouse back button.
m=$(mark); run "$POINTER" 1280 720 move $(at "item beta") click 272 pause 80 click 272 > /dev/null
wait_log "$m" "location $A/beta\$" || fail "a double click did not open beta"
m=$(mark); keys mod alt tap 105 mod none
wait_log "$m" "location $A\$" || fail "Alt+Left did not go back"
m=$(mark); keys mod alt tap 106 mod none
wait_log "$m" "location $A/beta\$" || fail "Alt+Right did not go forward"
m=$(mark); keys mod alt tap 103 mod none
wait_log "$m" "location $A\$" || fail "Alt+Up did not go up"
m=$(mark); keys mod alt tap 102 mod none
wait_log "$m" "location $HOME_DIR\$" || fail "Alt+Home did not go home"
m=$(mark); run "$POINTER" 1280 720 move $((WX + 400)) $((WY + 300)) click 275 > /dev/null
wait_log "$m" "location $A\$" || fail "the mouse back button did not go back"
echo "pass history"

# Go to Folder: Ctrl+L, type "delta", Enter.
run "$ANDROMEDA" settle > /dev/null
m=$(mark); keys mod control tap 38 mod none pause 200 tap 32 tap 18 tap 38 tap 20 tap 30 pause 200 tap 28
wait_log "$m" "location $A/delta\$" || fail "Ctrl+L, delta, Enter did not open delta"
m=$(mark); keys mod alt tap 105 mod none
wait_log "$m" "location $A\$" || fail "Alt+Left did not come back from delta"
echo "pass go to folder"

# Hidden files: Ctrl+H shows .secret.
m=$(mark); keys mod control tap 35 mod none
wait_log "$m" "geometry item \\.secret" || fail "Ctrl+H did not show .secret"
m=$(mark); keys mod control tap 35 mod none
wait_log "$m" "geometry item note\\.txt" || fail "a second Ctrl+H did not lay the folder out again"
echo "pass hidden"

# Quick Look and Get Info on note.txt.
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "item note.txt") click 272 > /dev/null
wait_log "$m" "cursor note.txt selected 1" || fail "a click did not select note.txt"
m=$(mark); keys tap 57
wait_log "$m" "quick look show" || fail "Space did not open Quick Look"
# The previewer is its own window (NautilusPreviewer): Escape goes to it; in Files a second Space closes it.
m=$(mark); keys tap 57
wait_log "$m" "quick look close" || fail "a second Space did not close Quick Look"
m=$(mark); keys mod alt tap 28 mod none
wait_log "$m" "get info 1 items" || fail "Alt+Enter did not open Get Info"
keys tap 1
run "$ANDROMEDA" settle > /dev/null
echo "pass quick look, get info"

# Middle click on a folder: a new tab on it.
m=$(mark); run "$POINTER" 1280 720 move $(at "item beta") click 274 > /dev/null
wait_log "$m" "tab new 2 of 2" || fail "a middle click on beta did not open a new tab"
wait_log "$m" "location $A/beta\$" || fail "the new tab did not open beta"
echo "pass middle click"

# Search field: Ctrl+F, Ctrl+V pastes the clipboard text; a right click opens the edit menu.
run wl-copy gamma
m=$(mark); keys mod control tap 33 mod none pause 200 mod control tap 47 mod none
wait_log "$m" 'filter "gamma" shows' || fail "Ctrl+F and Ctrl+V did not put the clipboard text in the search field"
m=$(mark); run "$POINTER" 1280 720 move $((WX + $(jq .w <<< "$w") - 260)) $((WY + 24)) click 273 > /dev/null
wait_log "$m" "popup configured" || fail "a right click on the search field opened no edit menu"
keys tap 1
# Middle click pastes the primary selection: "gamma" + "zzz" matches nothing.
run "$ANDROMEDA" settle > /dev/null
run wl-copy --primary zzz
m=$(mark); run "$POINTER" 1280 720 move $((WX + $(jq .w <<< "$w") - 260)) $((WY + 24)) click 274 > /dev/null
wait_log "$m" 'filter "(gamma)?zzz(gamma)?" shows 0' || fail "a middle click on the search field did not paste the primary selection"
run wl-copy --clear
echo "pass search field"

run grim "$OUT/actions-end.png"
stalls=$(awk 'match($0, /prepareFrame callback took [0-9.]+ms/) { n++; v = substr($0, RSTART + 27, RLENGTH - 29); if (n > 1 && v + 0 > 50) c++ } END { print c + 0 }' "$LOG")
echo "frame callbacks over 50 ms after the first: $stalls"
[[ $stalls -eq 0 ]] || fail "$stalls frame callbacks stalled the main thread over 50 ms"
echo "pass files_actions"
