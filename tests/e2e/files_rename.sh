#!/usr/bin/env bash
# Files v2 FB1 (FILES-UX.md §2.5, §6): inline rename, New Folder and the app undo stack (D10).
# Fails when:
#   - F2 does not open the field on the selected item, or typing does not replace only the stem,
#   - F2 twice does not select the whole name, a "/" does not show the inline error,
#   - Tab does not rename and move to the next item, Escape does not cancel,
#   - Ctrl+Z does not undo the rename, or Ctrl+Shift+N does not create "New Folder" in rename.
# Shots go to $OUT (default ./artifacts/files-rename).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-rename}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC"
for f in kilo.txt lima.txt mike.txt report.final.pdf; do echo "$f" > "$SRC/$f"; done

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
ENVS=(DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8)
run env "${ENVS[@]}" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run env "${ENVS[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
run "$ANDROMEDA" settle > /dev/null

wait_log() {
  for _ in $(seq 80); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0
    sleep 0.05
  done
  return 1
}
wait_file() {
  for _ in $(seq 80); do
    eval "$1" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
# evdev codes: F2 60, Enter 28, Tab 15, Esc 1, slash 53, z 44, n 49, letters below.
type_word() {
  local args=() ch
  declare -A code=([a]=30 [b]=48 [c]=46 [d]=32 [e]=18 [f]=33 [g]=34 [h]=35 [i]=23 [j]=36 [k]=37 [l]=38 [m]=50
    [n]=49 [o]=24 [p]=25 [q]=16 [r]=19 [s]=31 [t]=20 [u]=22 [v]=47 [w]=17 [x]=45 [y]=21 [z]=44 [/]=53)
  for ((i = 0; i < ${#1}; i++)); do
    ch=${1:i:1}
    args+=(tap "${code[$ch]}")
  done
  keys "${args[@]}"
}

wait_log 0 "location $SRC" || fail "the window did not list $SRC"
wait_log 0 "first frame" || fail "the window drew no first frame"
shot 0-start

# Stem only: report.final.pdf -> plan.pdf.
m=$(mark); type_word rep
wait_log "$m" "cursor report.final.pdf " || fail "type-ahead did not reach report.final.pdf"
sleep 1.1
m=$(mark); keys tap 60
wait_log "$m" "rename report.final.pdf" || fail "F2 did not open the rename field"
shot 1-field
type_word plan
m=$(mark); keys tap 28
wait_file '[[ -f $SRC/plan.pdf && ! -e $SRC/report.final.pdf ]]' || fail "the stem rename did not give plan.pdf"

# Undo (D10).
m=$(mark); keys mod control tap 44 mod none
wait_file '[[ -f $SRC/report.final.pdf && ! -e $SRC/plan.pdf ]]' || fail "Ctrl+Z did not undo the rename"

# F2 twice = the whole name; "/" = inline error; Escape cancels.
sleep 1.1
m=$(mark); type_word ki
wait_log "$m" "cursor kilo.txt " || fail "type-ahead did not reach kilo.txt"
sleep 1.1
m=$(mark); keys tap 60
wait_log "$m" "rename kilo.txt" || fail "F2 did not open the field on kilo.txt"
keys tap 60
type_word a/b
m=$(mark); keys tap 28
wait_log "$m" "rename error" || fail "a / did not show the inline error"
shot 2-error
keys tap 1
[[ -f $SRC/kilo.txt ]] || fail "Escape did not keep kilo.txt"

# Tab renames and moves on: kilo.txt -> kz.txt, then the field opens on lima.txt.
sleep 1.1
m=$(mark); type_word ki
wait_log "$m" "cursor kilo.txt " || fail "type-ahead did not reach kilo.txt again"
sleep 1.1
m=$(mark); keys tap 60
wait_log "$m" "rename kilo.txt" || fail "F2 did not open the field the second time"
type_word kz
m=$(mark); keys tap 15
wait_log "$m" "rename lima.txt" || fail "Tab did not move the field to lima.txt"
wait_file '[[ -f $SRC/kz.txt ]]' || fail "Tab did not rename kilo.txt to kz.txt"
keys tap 1

# New Folder opens in rename.
m=$(mark); keys mod control+shift tap 49 mod none
wait_file '[[ -d "$SRC/New Folder" ]]' || fail "Ctrl+Shift+N did not create New Folder"
wait_log "$m" "rename New Folder" || fail "the new folder did not open in rename"
type_word drafts
keys tap 28
wait_file '[[ -d $SRC/drafts && ! -e "$SRC/New Folder" ]]' || fail "the new folder was not renamed to drafts"
shot 3-end
echo "pass files_rename"
