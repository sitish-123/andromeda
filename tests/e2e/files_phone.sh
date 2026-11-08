#!/usr/bin/env bash
# Files FB-PHONE (FILES-UI-SPEC §20): the phone place and page for a paired Link phone, and Import.
# The phone is tests/e2e/fake_link_daemon.py on a private bus. Fails when:
#   - the sidebar has no phone row with its battery,
#   - the page does not show 3 recent photos, the browse folder (~/Phone/Pixel E2E) and the received file,
#   - Import of one picked photo does not save it into ~/Pictures/Pixel E2E/<date>, or Import All copies an imported
#     photo again, or a second Import All copies anything,
#   - a file dragged onto the phone row is not sent to the phone, or the transfer gets no job pill row,
#   - a phone that holds back photo access does not show Photos Off.
# SCALE=2 writes the 2x shots. Shots and logs go to $OUT (default ./artifacts/files-phone).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-phone}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
mkdir -p "$OUT" "$HOME_DIR/Downloads" "$HOME_DIR/Phone/Pixel E2E/DCIM" "$HOME_DIR/Phone/Pixel E2E/Download"
fail() { echo "FAIL: $*"; exit 1; }
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi
echo "from the phone" > "$HOME_DIR/Downloads/boarding-pass.pdf"
setfattr -n user.xdg.origin.url -v "link://e2e-phone" "$HOME_DIR/Downloads/boarding-pass.pdf"
echo "not from the phone" > "$HOME_DIR/Downloads/other.txt"
echo "send me" > "$HOME_DIR/note.txt"
NOTE_SIZE=$(stat -c %s "$HOME_DIR/note.txt")

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

wait_for() { local what=$1; shift; for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
logged() { grep -q -- "$1" "$OUT/files.log"; }
link_logged() { grep -q -- "$1" "$OUT/link.log"; }
start_link() { # [env assignments]
  : > "$OUT/link.log"
  env DBUS_SESSION_BUS_ADDRESS="$BUS" "$@" python3 -u "$ROOT/tests/e2e/fake_link_daemon.py" >> "$OUT/link.log" 2>&1 &
  LINK_PID=$!
  wait_for "the fake Link daemon did not start" link_logged "^READY"
}
start_link
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" \
  NOCTALIA_LOG_LEVEL=debug DESK_FILES_GEOMETRY=1 "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
wait_for "the Files window did not map" mapped
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
shot() { run "$ANDROMEDA" settle > /dev/null; sleep 0.3; run grim "$OUT/$1.png"; }

wait_for "the sidebar has no phone row with 82 %" logged "sidebar phone:e2e-phone 82"
echo "pass sidebar"

action phone.open "[<'e2e-phone'>]"
wait_for "the phone page did not open" logged "phone page e2e-phone"
wait_for "the page did not get 3 photos" logged "phone e2e-phone photos 3"
wait_for "the page does not show 3 photos, 2 folders and 1 received file: $(grep -o 'phone page e2e-phone strip.*' "$OUT/files.log" | tail -n 1)" \
  logged "phone page e2e-phone strip 3 folders 2 received 1"
shot page
echo "pass page"

DEST=$HOME_DIR/Pictures/"Pixel E2E"/$(date -u +%F)
saves() { grep -c "^SAVE " "$OUT/link.log" || true; }
action phone.import-view
wait_for "the Import view did not open" logged "phone import view e2e-phone"
action phone.pick "[<'0'>]"
wait_for "the pick did not register" logged "phone picked 1"
shot import
action phone.import
wait_for "Import did not save IMG_0003.jpg into $DEST" test -f "$DEST/IMG_0003.jpg"
wait_for "Import did not finish" logged "phone imported 1 photos"
[[ $(saves) -eq 1 ]] || fail "Import of one photo saved $(saves)"
action phone.import "[<'all'>]"
wait_for "Import All did not finish" logged "phone imported 2 photos"
[[ -f $DEST/IMG_0002.jpg && -f $DEST/IMG_0001.jpg ]] || fail "Import All did not save the other two photos"
[[ $(saves) -eq 3 ]] || fail "Import All copied an imported photo again ($(saves) saves)"
action phone.import "[<'all'>]"
run "$ANDROMEDA" settle > /dev/null
sleep 0.5
[[ $(saves) -eq 3 ]] || fail "a second Import All copied photos again ($(saves) saves)"
shot imported
echo "pass import"

# Drag note.txt from the home folder onto the phone row: a Link transfer with a job pill row.
action go "[<'$HOME_DIR'>]"
wait_for "the window did not go home" logged "location $HOME_DIR"
wait_for "no geometry for note.txt" logged "geometry item note.txt"
run "$ANDROMEDA" settle > /dev/null
w=$(run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0]")
WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w")
at() { local line; line=$(grep -E "geometry $1( |$)" "$OUT/files.log" | tail -n 1); [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"; echo "$((WX + x)) $((WY + y))"; }
from=($(at "item note.txt")); to=($(at "place Pixel E2E"))
run "$POINTER" 1280 720 tap 119 pause 100 move "${from[0]}" "${from[1]}" pause 450 press 272 \
  move $((from[0] + 6)) $((from[1] + 6)) move $((from[0] + 20)) $((from[1] + 20)) \
  move $(((from[0] + to[0]) / 2)) $(((from[1] + to[1]) / 2)) move "${to[0]}" "${to[1]}" \
  pause 150 move $((to[0] + 1)) "${to[1]}" pause 50 release 272 > /dev/null
wait_for "the drop onto the phone row did not run" logged "drop 1 items onto phone:e2e-phone"
wait_for "the phone did not get note.txt ($NOTE_SIZE bytes)" link_logged "^SEND e2e-phone note.txt:$NOTE_SIZE\$"
wait_for "the transfer got no job pill row" logged "job pill 1"
shot sending
echo "pass drag to phone"

kill "$LINK_PID"
wait "$LINK_PID" 2> /dev/null || true
start_link FAKE_LINK_PHOTOS=denied
action phone.open "[<'e2e-phone'>]"
wait_for "the page did not show Photos Off" logged "photos-off"
shot photos-off
echo "pass photos off"
echo "pass files_phone"
