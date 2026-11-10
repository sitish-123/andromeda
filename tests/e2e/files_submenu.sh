#!/usr/bin/env bash
# Files item menu, pointer on a nested submenu row (user, 2026-11-08: a touchpad tap on Open With › Player did
# nothing). A touchpad tap with tap-and-drag sends the press at once and the release about 200 ms later. Fails when:
#   - a click on the Actions › E2E › Mark for E2E row does not run the service menu,
#   - a press, a 250 ms pause and a release on that row (the tap timing) does not run it.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-submenu}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
A=$HOME_DIR/Alpha
boot_headless 1
mkdir -p "$OUT" "$A" "$HOME_DIR/.local/share/kio/servicemenus"
fail() { echo "FAIL: $*"; exit 1; }
echo one > "$A/note.txt"
MARKS=$OUT/service-marks
cat > "$HOME_DIR/.local/share/kio/servicemenus/e2e.desktop" <<EOF
[Desktop Entry]
Type=Service
MimeType=text/plain;
Actions=mark
X-KDE-Submenu=E2E

[Desktop Action mark]
Name=Mark for E2E
Exec=sh -c 'for f in "\$@"; do echo "\$f" >> $MARKS; done' _ %F
EOF

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

LOG=$OUT/files.log
start() {
  : > "$LOG"
  run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 ANDROMEDA_LOG_LEVEL=debug \
    DESK_FILES_GEOMETRY=1 "$FILES" "$A" >> "$LOG" 2>&1 &
  FILES_PID=$!
  for _ in $(seq 100); do
    w=$(run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty")
    [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
    sleep 0.05
  done
  [[ -n ${w:-} ]] || fail "the Files window did not map"
  WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w")
  run "$ANDROMEDA" settle > /dev/null
}
mark() { wc -l < "$LOG"; }
wait_log() { for _ in $(seq 100); do grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$LOG") && return 0; sleep 0.05; done; return 1; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
at() { local line; line=$(grep -E "geometry $1( |$)" "$LOG" | tail -n 1); [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"; echo "$((WX + x)) $((WY + y))"; }

# Opens Actions › E2E with the keyboard (as files_viewopts.sh does), then prints the centre of the deepest popup, whose
# only row is Mark for E2E. Popup positions chain: the first is window-relative, each submenu is parent-surface relative.
open_row() {
  local m item
  m=$(mark); item=$(at "item note.txt")
  run "$POINTER" 1280 720 move $item click 273 > /dev/null
  wait_log "$m" "popup configured" || fail "the right click did not open the item menu"
  run "$ANDROMEDA" settle > /dev/null
  keys tap 103 tap 103 tap 106 pause 300 tap 106 pause 300
  run "$ANDROMEDA" settle > /dev/null
  mapfile -t pops < <(tail -n +"$((m + 1))" "$LOG" | grep -oE "popup configured -?[0-9]+ -?[0-9]+ [0-9]+x[0-9]+" | tail -n 3)
  [[ ${#pops[@]} -eq 3 ]] || fail "the menu, Actions and E2E popups did not all open (${#pops[@]})"
  local x=$WX y=$WY px py w h
  for p in "${pops[@]}"; do
    read -r _ _ px py size <<< "$p"
    x=$((x + px)); y=$((y + py)); w=${size%x*}; h=${size#*x}
  done
  echo "$((x + w / 2)) $((y + h / 2))"
}

start
: > "$MARKS"
row=$(open_row) || fail "the submenu did not open"
echo "window $WX $WY, submenu row at $row"
run grim "$OUT/1-submenu-open.png"
run "$POINTER" 1280 720 move $row pause 100 click 272 > /dev/null
for _ in $(seq 60); do [[ -s $MARKS ]] && break; sleep 0.05; done
[[ -s $MARKS ]] || fail "a click on Actions › E2E › Mark for E2E did not run it"
echo "pass click"

: > "$MARKS"
row=$(open_row) || fail "the submenu did not open"
run "$POINTER" 1280 720 move $row pause 100 press 272 pause 250 release 272 > /dev/null
for _ in $(seq 60); do [[ -s $MARKS ]] && break; sleep 0.05; done
[[ -s $MARKS ]] || fail "a press, 250 ms and a release (touchpad tap timing) on the submenu row did not run it"
echo "pass files_submenu"
