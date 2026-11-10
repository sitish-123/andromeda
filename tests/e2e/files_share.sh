#!/usr/bin/env bash
# Files item menu Share › <phone> (mockup §4): a right click on note.txt, Share, the Link phone "Pixel E2E" sends the
# file through org.andromeda.Link1 SendFiles (tests/e2e/fake_link_daemon.py on a private bus).
# Fails when:
#   - the menu row does not run share.device:e2e-phone,
#   - the daemon does not get note.txt with its full size,
#   - Files does not log the sent result.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-share}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT" "$SRC"
fail() { echo "FAIL: $*"; exit 1; }
printf 'share me\n' > "$SRC/note.txt"
SIZE=$(stat -c %s "$SRC/note.txt")

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

wait_file() { # file pattern
  for _ in $(seq 100); do grep -qE "$2" "$1" 2> /dev/null && return 0; sleep 0.05; done
  return 1
}
DBUS_SESSION_BUS_ADDRESS=$BUS python3 -u "$(dirname "$0")/fake_link_daemon.py" > "$OUT/link.log" 2>&1 &
wait_file "$OUT/link.log" "^READY" || fail "the fake Link daemon did not start"

run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 ANDROMEDA_LOG_LEVEL=debug DESK_FILES_GEOMETRY=1 \
  "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &
for _ in $(seq 100); do
  w=$(run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty")
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
WX=$(jq .x <<< "$w")
WY=$(jq .y <<< "$w")
wait_file "$OUT/files.log" "geometry item note.txt" || fail "no geometry for note.txt"
run "$ANDROMEDA" settle > /dev/null
read -r x y <<< "$(grep -E "geometry item note.txt( |$)" "$OUT/files.log" | tail -n 1 | awk '{print $(NF-1), $NF}')"

run "$POINTER" 1280 720 move $((WX + x)) $((WY + y)) click 273 > /dev/null
wait_file "$OUT/files.log" "selected 1 of " || fail "the right click did not select note.txt"
run "$ANDROMEDA" settle > /dev/null
# The highlight starts on Open and wraps: Up x4 = Move to Trash, Open Terminal Here, Add to Starred, Share.
run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 tap 103 tap 103 tap 103 tap 103 tap 106 pause 300 tap 28 > /dev/null
wait_file "$OUT/files.log" "menu share.device:e2e-phone" || fail "the Share › Pixel E2E row did not run"
wait_file "$OUT/link.log" "^SEND e2e-phone note.txt:$SIZE\$" || fail "the Link daemon did not get note.txt ($SIZE bytes)"
wait_file "$OUT/files.log" "share result sent" || fail "Files did not log the sent result"
echo "pass files_share"
