#!/usr/bin/env bash
# Files v2 FB-ARCH (mockup section 8): an archive opens like a folder, its folders open inside it, and Extract Here
# lands the items next to the archive (two top items go into a folder named after it).
# Fails when:
#   - Enter on pack.zip does not list the archive, or Enter on its docs folder does not list docs inside it,
#   - Alt+Left does not go back to the archive top,
#   - Extract All on the archive bar does not create pack/docs/alpha.txt and pack/readme.md.
# Shots go to $OUT (default ./artifacts/files-archive).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-archive}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC"
python3 -I - "$SRC/pack.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("docs/alpha.txt", "a")
    z.writestr("docs/beta.txt", "b")
    z.writestr("readme.md", "r")
PY

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug DESK_MOTION_TRACE=1 DESK_FRAME_STATS=1 \
  DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
  "$FILEOPS" > "$OUT/fileops.log" 2>&1 &

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
wait_log 0 "first frame" || fail "the window drew no first frame"

m=$(mark); keys tap 25 tap 30 tap 28            # p a, Enter: open pack.zip
wait_log "$m" "archive pack.zip " || fail "Enter on pack.zip did not list the archive"
wait_log "$m" "location archive:" || fail "the archive listing did not show"
wait_log "$m" "geometry item docs " || fail "the archive top has no docs row"
shot 1-archive

sleep 1.1
m=$(mark); keys tap 32 tap 28                   # d, Enter: docs inside the archive
wait_log "$m" "location archive:.*docs" || fail "Enter on docs did not open it inside the archive"
wait_log "$m" "geometry item alpha.txt " || fail "docs inside the archive does not list alpha.txt"
shot 2-docs

m=$(mark); keys mod alt tap 105 mod none        # Alt+Left: the archive top again
wait_log "$m" "location archive:" || fail "Alt+Left did not go back to the archive top"
wait_log "$m" "settle files.page.under" || fail "the slide back to the archive top did not settle"
sleep 0.1 # the geometry after the last settle
wait_log "$m" "geometry banner-action 1 " || fail "the archive bar has no Extract All button"

m=$(mark); run "$POINTER" 1280 720 move $(at "banner-action 1") click 272 > /dev/null
[[ -n $(timeout 8 bash -c "until [[ -f '$SRC/pack/docs/alpha.txt' && -f '$SRC/pack/readme.md' ]]; do sleep 0.05; done; echo ok") ]] \
  || fail "Extract All did not land pack/docs/alpha.txt and pack/readme.md"
shot 3-extracted
echo "pass files_archive"
