#!/usr/bin/env bash
# The Screen page of the Qt Link app (andromeda-link) mirrors inside the page: against a private andromeda-linkd and the
# headless phone, which streams an H.264 file of SMPTE bars as the app streams its encoder's output. Proves:
# 1. Mirror the screen shows the bars inside the page, with no other window.
# 2. A click on the picture reaches the phone as a tap at that point, a drag as a swipe, and typed text as text.
# 3. Stop tells the phone to stop, and the page goes back to its button.
# Writes screen-idle.png, screen-live.png, screen-stopped.png, steps.txt, hold.jsonl, linkd.log, app.log to $OUT
# (default ./artifacts/link-desk-mirror).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-desk-mirror}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/hold.jsonl" "$OUT/app.log"

gst-launch-1.0 -q videotestsrc num-buffers=60 pattern=smpte \
  ! video/x-raw,width=720,height=1280,framerate=30/1 ! x264enc tune=zerolatency key-int-max=30 aud=true \
  ! video/x-h264,stream-format=byte-stream,profile=baseline ! filesink location="$RUNTIME/bars.h264"

with_noctalia '
  BIN='"$LINK_BIN"'
  POINTER='"$POINTER"'
  fail() { grim "$OUT/fail.png" || true; echo "FAIL: $*" >&2; exit 1; }
  step() { printf "%s\n" "$*" | tee -a "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 200); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
  shot() { sleep 1.5; grim "$OUT/$1.png"; } # real time: the window repaints
  click() { "$POINTER" 1280 720 move "$1" "$2" click 272 > /dev/null; }
  events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  CODE=$(link StartPairing | sed -E "s/^\(.([0-9]+)., .*/\1/")
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
    > /dev/null 2>> "$OUT/phone.log" || fail "pairing"
  ID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"peers\"][0][\"id\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  link SetGrant "$ID" screen true > /dev/null
  python3 -c "import json,sys; s=json.load(open(sys.argv[1])); s[\"peers\"][0].setdefault(\"grants\", {})[\"screen\"]=True; json.dump(s, open(sys.argv[1], \"w\"))" "$RUNTIME/phone/devices.json"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" --mirror-file "$RUNTIME/bars.h264" \
    --mirror-size 720x1280 hold > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "the phone never connected" grep -q "\"event\":\"connected\"" "$OUT/hold.jsonl"

  QT_FORCE_STDERR_LOGGING=1 "$BIN/andromeda-link" >> "$OUT/app.log" 2>&1 &
  APP=$!
  sleep 2 # real time: the window maps
  # 1280x720 points: Screen in the sidebar, Mirror the screen, the picture (634..886 x 132..582 for 720x1280), Stop.
  click 143 367
  shot screen-idle
  click 759 605
  wait_for "the mirror never started" grep -q "mirroring at" "$OUT/linkd.log"
  shot screen-live
  [[ -z $(pgrep -P "$APP" || true) ]] || fail "the app started a separate mirror: $(pgrep -a -P "$APP")"
  pixel() { magick "$OUT/screen-live.png" -format "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]" -crop "1x1+$1+$2" info:; }
  YELLOW=$(pixel 688 250) BLUE=$(pixel 868 250)
  python3 - "$YELLOW" "$BLUE" <<PY || fail "the page does not show the bars: yellow=$YELLOW blue=$BLUE"
import sys
yellow, blue = ([int(c) for c in arg.split(",")] for arg in sys.argv[1:])
assert yellow[0] > 150 and yellow[1] > 150 and yellow[2] < 60, yellow
assert blue[2] > 150 and blue[0] < 60 and blue[1] < 60, blue
PY
  step "live: the bars show inside the page (yellow $YELLOW, blue $BLUE)"

  click 760 357
  "$POINTER" 1280 720 move 760 500 press 272 move 760 420 move 760 300 move 760 200 release 272 > /dev/null
  "$POINTER" 1280 720 move 760 357 notch 1 > /dev/null
  wtype -s 300 hi
  wait_for "the input did not all reach the phone" eval "(( \$(events mirror-input) >= 5 ))"
  grep "\"event\":\"mirror-input\"" "$OUT/hold.jsonl" > "$OUT/input.jsonl"
  python3 - "$OUT/input.jsonl" <<PY || fail "the phone got other input: $(cat "$OUT/input.jsonl")"
import json, sys
got = [json.loads(line) for line in open(sys.argv[1])]
tap, swipe, scroll = (next(e for e in got if e["action"] == a) for a in ("tap", "swipe", "scroll"))
assert abs(tap["x"] - 5000) < 150 and abs(tap["y"] - 5000) < 150, tap
assert abs(swipe["x"] - 5000) < 150 and swipe["y"] > 7500 and swipe["y2"] < 2000, swipe
assert scroll["y2"] < 0, scroll
assert "".join(e["text"] for e in got if e["action"] == "text") == "hi", got
PY
  step "input: tap, swipe, scroll, and typed text reach the phone: $(tr -d "\n" < "$OUT/input.jsonl" | head -c 400)"

  click 909 650
  wait_for "Stop did not stop the phone" grep -q "\"event\":\"mirror-stop\".*viewer closed" "$OUT/hold.jsonl"
  shot screen-stopped
  step "stopped: Stop ended the phone stream"
'
