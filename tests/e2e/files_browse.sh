#!/usr/bin/env bash
# Files v2 FB0 (FILES-UX.md §6): the FileBrowser in the Files window. Keys open a folder (Slide on spring.page),
# go back and to the parent, switch the view (fade) and type-ahead select; every step waits on the app log.
# Fails when:
#   - type-ahead "rep" does not put the cursor on report.pdf, or a repeated "r" does not cycle on from it,
#   - Enter on a folder, Alt+Left or Backspace do not change the location, or the folder slide is not traced on page,
#   - Ctrl+2 does not trace the view crossfade, or an animation reports missed frames (DESK_FRAME_STATS),
#   - Ctrl+F and typing do not filter the folder, or Escape does not clear it.
# Shots of the icons and list views go to $OUT (default ./artifacts/files-browse).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-browse}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC/Andromeda/inner"
for f in budget.ods report.pdf roadmap.md Résumé.pdf zeta.txt; do echo x > "$SRC/$f"; done
echo y > "$SRC/Andromeda/notes.md"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug DESK_MOTION_TRACE=1 DESK_FRAME_STATS=1 \
  "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
run "$ANDROMEDA" settle > /dev/null

# Waits until a line matching $2 appears in the log after line $1.
wait_log() {
  for _ in $(seq 60); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
# The first key of a new virtual keyboard only gives it focus: Pause goes first.
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }

wait_log 0 "location $SRC" || fail "the window did not list $SRC"
wait_log 0 "first frame" || fail "the window drew no first frame"
shot 1-icons

m=$(mark); keys tap 19 tap 18 tap 25            # r e p
wait_log "$m" "cursor report.pdf " || fail "type-ahead rep did not reach report.pdf"
sleep 1.1                                       # the buffer resets after 1 s
m=$(mark); keys tap 19 tap 19                   # C.UTF-8 order puts Résumé.pdf before budget.ods: r r = roadmap.md, wrap to Résumé.pdf
wait_log "$m" "cursor roadmap.md " || fail "r after report.pdf did not move on to roadmap.md"
wait_log "$m" "cursor Résumé.pdf " || fail "a repeated r did not wrap around to Résumé.pdf"

sleep 1.1
m=$(mark); keys tap 30 tap 28                   # a, Enter: open Andromeda
wait_log "$m" "location $SRC/Andromeda\$" || fail "Enter did not open Andromeda"
wait_log "$m" "motion files.page page" || fail "opening a folder did not slide on spring.page"
wait_log "$m" "settle files.page " || fail "the folder slide did not settle"

m=$(mark); keys mod alt tap 105 mod none        # Alt+Left: back
wait_log "$m" "location $SRC\$" || fail "Alt+Left did not go back"
wait_log "$m" "cursor Andromeda |selected 1 of 1 in $SRC|motion files.page" || fail "going back did not animate"

m=$(mark); keys tap 30 tap 28                   # into Andromeda again, then Backspace: parent
wait_log "$m" "location $SRC/Andromeda\$" || fail "Enter did not open Andromeda the second time"
m=$(mark); keys tap 14
wait_log "$m" "location $SRC\$" || fail "Backspace did not go to the parent"
wait_log "$m" "settle files.page" || fail "the parent slide did not settle"

m=$(mark); keys mod control tap 3 mod none       # Ctrl+2: list
wait_log "$m" "motion files.view fade" || fail "Ctrl+2 did not crossfade the view"
wait_log "$m" "settle files.view " || fail "the view crossfade did not settle"
shot 2-list

m=$(mark); keys mod control tap 33 mod none tap 19 tap 24 tap 30 tap 32   # Ctrl+F, r o a d
wait_log "$m" 'filter "road" shows 1$' || fail "the search field did not filter the folder to roadmap.md"
shot 3-filter
m=$(mark); keys tap 1 tap 1 tap 19               # Escape clears, Escape again leaves the field; r = type-ahead
wait_log "$m" "cursor (R|r)" || fail "Escape did not clear the filter and give the keys back to the list"

grep -E "anim .* missed [1-9]" "$OUT/files.log" && fail "an animation missed frames"
grep -qE "anim .* frames [0-9]+" "$OUT/files.log" || fail "DESK_FRAME_STATS reported no animation"
grep -E "first frame|anim " "$OUT/files.log" | tail -n 8
echo "pass files_browse"
