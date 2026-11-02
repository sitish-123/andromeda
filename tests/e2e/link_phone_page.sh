#!/usr/bin/env bash
# Control Center phone page (Link in the shell), against a real andromeda-linkd and a present headless phone: the page
# opens from the quick sheet, shows the phone, and its Ring row makes the phone ring; a phone that goes away shows
# Not connected. Shots go to $OUT (default ./artifacts/link-phone-page).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-phone-page}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  phone_saw() { [[ $(grep -c "$1" "$OUT/hold.jsonl") -ge ${2:-1} ]]; }
  ringing() { [[ $(msg link-ringing) == "$1" ]]; }
  click() { "$POINTER" 1280 720 move "$1" "$2" > /dev/null; "$POINTER" 1280 720 move "$1" "$2" click 272 move 5 715 > /dev/null; }

  XDG_STATE_HOME=$RUNTIME/linkd XDG_DOWNLOAD_DIR=$RUNTIME/downloads "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  # Four photos in the phone library, and the phone lets this desktop browse.
  LIBRARY=$RUNTIME/library
  mkdir -p "$LIBRARY/Camera" "$RUNTIME/downloads"
  python3 -I - "$LIBRARY" <<PY
import os, struct, sys, time, zlib
def png(path, seed, age):
    rows = b"".join(b"\0" + bytes((x * seed + y) % 256 for x in range(3 * 16)) for y in range(16))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    t = time.time() - age
    os.utime(path, (t, t))
for name, seed, age in (("newest", 7, 100), ("second", 5, 200), ("third", 3, 300), ("fourth", 9, 400)):
    png(os.path.join(sys.argv[1], "Camera", name + ".png"), seed, age)
PY
  python3 -I - "$RUNTIME/phone/devices.json" <<PY
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["browse"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
  msg theme-mode-set "${THEME:-light}" > /dev/null
  msg panel-open control-center quick > /dev/null
  sleep 1 # real time: the sheet opens
  grim "$OUT/phone-disconnected-card.png"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --photos "$LIBRARY" hold --status 82,0,wifi \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  sleep 1 # real time: the card repaints
  grim "$OUT/phone-card.png"

  msg panel-open control-center detail-phone > /dev/null
  # The tiles are flat grey until the thumbnails arrive; a loaded gradient spans a wide range of grey.
  thumbs_ready() {
    grim "$OUT/now.png" && python3 -I - "$OUT/now.png" <<PY
import sys
from PIL import Image
lo, hi = Image.open(sys.argv[1]).convert("L").crop((930, 292, 1262, 350)).getextrema()
sys.exit(0 if hi - lo > 100 else 1)
PY
  }
  wait_for "the four photo thumbnails did not arrive" thumbs_ready
  sleep 1 # real time: the page repaints with the thumbnails
  grim "$OUT/phone-connected.png"
  # A photo saves to Downloads; the newest photo is the first thumbnail.
  click "${PHOTO_X:-962}" "${PHOTO_Y:-320}"
  wait_for "the photo was not saved to Downloads" cmp -s "$LIBRARY/Camera/newest.png" "$RUNTIME/downloads/newest.png"
  sleep 1 # real time: the confirmation shows
  grim "$OUT/phone-photo-saved.png"
  click "${RING_X:-1000}" "${RING_Y:-180}"
  wait_for "pressing Ring did not ring the phone" phone_saw "\"event\":\"ring\",\"on\":true"
  wait_for "the shell does not show the phone ringing" ringing "phone $ID"
  click "${RING_X:-1000}" "${RING_Y:-180}"
  wait_for "pressing Stop did not stop the phone" phone_saw "\"event\":\"ring\",\"on\":false"

  notify-send -a Messages -h "string:x-andromeda-group:$ID/messages" "Maya" "Are you coming tonight?"
  sleep 1 # real time: the page repaints
  grim "$OUT/phone-notifications.png"

  # A click on a phone call in the center island opens the sheet on this page.
  msg panel-close > /dev/null
  wait_for "the sheet did not close: $(msg panel-state)" eval "[[ \$(msg panel-state) == none ]]"
  say() { printf "%s\n" "$*" >&3; }
  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0100\",\"name\":\"Ada Lovelace\"}"
  wait_for "the call is not a live activity" eval "[[ \$(msg live-activity) == *Ada* ]]"
  sleep 1 # real time: the island opens
  grim "$OUT/phone-call-island.png"
  click "${ISLAND_X:-560}" "${ISLAND_Y:-24}"
  sleep 1 # real time: the sheet opens
  grim "$OUT/phone-from-island.png"
  wait_for "the island click did not open the phone page: $(msg panel-state)" eval "[[ \$(msg panel-state) == \"control-center detail-phone\" ]]"

  # The header toggle pauses and resumes the phone here.
  paused() { python3 -I -c "import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))[\"peers\"][0].get(\"paused\") else 1)" "$RUNTIME/linkd/andromeda-link/devices.json"; }
  not_paused() { ! paused; }
  click "${TOGGLE_X:-1239}" "${TOGGLE_Y:-83}"
  wait_for "the toggle did not pause the phone" paused
  sleep 1 # real time: the page repaints
  grim "$OUT/phone-paused.png"
  click "${TOGGLE_X:-1239}" "${TOGGLE_Y:-83}"
  wait_for "the toggle did not resume the phone" not_paused

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still shown connected" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" || \$(msg link-devices) != *\" connected \"* ]]"
  sleep 1 # real time: the page repaints
  grim "$OUT/phone-disconnected.png"
'
echo "PASS; artifacts: $OUT"
