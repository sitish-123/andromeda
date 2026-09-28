#!/usr/bin/env bash
# Mirroring a phone's screen, headless: the phone streams an H.264 file of SMPTE bars (umbriel-link-phone
# --mirror-file, as the app streams its encoder's output) and umbriel-link-mirror --frames decodes what OpenMirror
# hands it into PNG files.
#
# Proves:
# 1. With the phone's screen switch off, as after pairing, OpenMirror is refused with the phone's reason.
# 2. With it on, the viewer decodes 60 frames of the bars, at the phone's size and in the bars' colours.
# 3. Input reaches the phone as the viewer sends it: a tap, a swipe, a scroll, text, and Back, in 1/10000 units; an
#    out-of-range tap is refused before it is sent; a keyframe request reaches it too.
# 4. A second OpenMirror while one runs is refused; closing the viewer tells the phone to stop.
# Writes results.jsonl, frames/, hold.jsonl, hold.log, viewer.log, linkd.log to $OUT (default ./artifacts/link-mirror).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-mirror}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-mirror.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}

ip link set lo up
ip link add d0 type veth peer name p0
unshare --net sleep infinity &
PHONE_NS=$!
for _ in $(seq 50); do [[ $(readlink /proc/$PHONE_NS/ns/net) != "$(readlink /proc/self/ns/net)" ]] && break; sleep 0.02; done
ip link set p0 netns "$PHONE_NS"
ip addr add 10.81.0.1/24 dev d0
ip link set d0 up
in_phone() { nsenter -t "$PHONE_NS" -n -- "$@"; }
in_phone ip link set lo up
in_phone ip addr add 10.81.0.2/24 dev p0
in_phone ip link set p0 up

cat > "$RUNTIME/bus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$RUNTIME/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --config-file="$RUNTIME/bus.conf" --nofork 2> "$OUT/dbus.log" &
for _ in $(seq 50); do [[ -S $RUNTIME/bus ]] && break; sleep 0.02; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/bus

link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
XDG_STATE_HOME=$RUNTIME/desktop RUST_LOG=info "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
phone() { in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone "$@"; }
viewer() { "$BIN/umbriel-link-mirror" "$ID" --frames "$OUT/frames" "$@"; }
events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }

gst-launch-1.0 -q videotestsrc num-buffers=60 pattern=smpte \
  ! video/x-raw,width=720,height=1280,framerate=30/1 ! x264enc tune=zerolatency key-int-max=30 aud=true \
  ! video/x-h264,stream-format=byte-stream,profile=baseline ! filesink location="$RUNTIME/bars.h264"
[[ -s $RUNTIME/bars.h264 ]] || fail "could not encode the test video"

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.81.0.1:$PORT" > /dev/null 2>> "$OUT/hold.log" || fail "pairing"
ID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["peers"][0]["id"])' "$RUNTIME/desktop/umbriel-link/devices.json")
hold() {
  phone --mirror-file "$RUNTIME/bars.h264" --mirror-size 720x1280 hold >> "$OUT/hold.jsonl" 2>> "$OUT/hold.log" &
  HOLD=$!
  wait_for 20 "the phone never connected" grep -q '"event":"connected"' "$OUT/hold.jsonl"
}

hold
REFUSED=$(link OpenMirror "'$ID'" 2>&1 || true)
[[ $REFUSED == *Refused*"screen switch"* ]] || fail "OpenMirror with the screen switch off: $REFUSED"
record '{"step":"refused-with-switch-off"}'
# $! is the function's subshell; the phone itself runs under it.
pkill -f -- "--state $RUNTIME/phone .*hold" || true
wait "$HOLD" 2> /dev/null || true
python3 - "$RUNTIME/phone/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["screen"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
: > "$OUT/hold.jsonl"
hold

STARTED=$(date +%s.%N)
viewer --count 60 2> "$OUT/viewer.log" || fail "the viewer failed: $(tail -3 "$OUT/viewer.log")"
SECONDS_TAKEN=$(python3 -c "import time; print(round(time.time() - $STARTED, 2))")
FRAMES=$(ls "$OUT/frames" | wc -l)
(( FRAMES >= 55 )) || fail "only $FRAMES frames decoded"
SIZE=$(magick identify -format '%wx%h' "$OUT/frames/frame-030.png")
[[ $SIZE == 720x1280 ]] || fail "frames are $SIZE, not the phone's 720x1280"
pixel() { magick "$OUT/frames/frame-030.png" -format "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]" -crop "1x1+$1+300" info:; }
LEFT=$(pixel 40) YELLOW=$(pixel 140) BLUE=$(pixel 680)
python3 - "$LEFT" "$YELLOW" "$BLUE" <<'PY' || fail "frames are not the bars: white=$LEFT yellow=$YELLOW blue=$BLUE"
import sys
grey, yellow, blue = ([int(c) for c in arg.split(",")] for arg in sys.argv[1:])
assert all(c > 200 for c in grey), grey
assert yellow[0] > 150 and yellow[1] > 150 and yellow[2] < 60, yellow
assert blue[2] > 150 and blue[0] < 60 and blue[1] < 60, blue
PY
wait_for 10 "closing the viewer did not stop the phone" grep -q '"event":"mirror-stop".*viewer closed' "$OUT/hold.jsonl"
record "{\"step\":\"decodes-bars\",\"frames\":$FRAMES,\"size\":\"$SIZE\",\"seconds\":$SECONDS_TAKEN}"
record '{"step":"viewer-close-stops-phone"}'

rm -rf "$OUT/frames"
RUNS=$(grep -c "mirror\] .*mirroring at" "$OUT/linkd.log")
viewer --count 600 2>> "$OUT/viewer.log" &
VIEWER=$!
wait_for 20 "the second mirror never started" eval '(( $(grep -c "mirror\] .*mirroring at" "$OUT/linkd.log") > RUNS ))'
BUSY=$(link OpenMirror "'$ID'" 2>&1 || true)
[[ $BUSY == *"already mirroring"* ]] || fail "a second OpenMirror was not refused: $BUSY"
record '{"step":"second-open-refused"}'
link MirrorInput "'$ID'" tap "{'x': <int32 5000>, 'y': <int32 2500>}" > /dev/null
link MirrorInput "'$ID'" swipe "{'x': <int32 5000>, 'y': <int32 8000>, 'x2': <int32 5000>, 'y2': <int32 2000>, 'ms': <uint32 300>}" > /dev/null
link MirrorInput "'$ID'" scroll "{'x': <int32 5000>, 'y': <int32 5000>, 'x2': <int32 0>, 'y2': <int32 -1500>}" > /dev/null
link MirrorInput "'$ID'" text "{'text': <'hi'>}" > /dev/null
link MirrorInput "'$ID'" back "{}" > /dev/null
link MirrorKeyframe "'$ID'" > /dev/null
OUTSIDE=$(link MirrorInput "'$ID'" tap "{'x': <int32 20000>, 'y': <int32 5000>}" 2>&1 || true)
[[ $OUTSIDE == *Rejected* ]] || fail "an out-of-range tap was not refused: $OUTSIDE"
wait_for 10 "the input did not all reach the phone" eval '(( $(events mirror-input) >= 5 ))'
python3 - "$OUT/hold.jsonl" <<'PY' || fail "the phone got other input: $(grep mirror-input "$OUT/hold.jsonl")"
import json, sys
got = [json.loads(line) for line in open(sys.argv[1]) if '"mirror-input"' in line]
want = [
    ("tap", 5000, 2500, None, None, None, None),
    ("swipe", 5000, 8000, 5000, 2000, 300, None),
    ("scroll", 5000, 5000, 0, -1500, None, None),
    ("text", None, None, None, None, None, "hi"),
    ("back", None, None, None, None, None, None),
]
assert [(e["action"], e["x"], e["y"], e["x2"], e["y2"], e["ms"], e["text"]) for e in got] == want, got
PY
grep -q '"type":"mirror-keyframe"' "$OUT/hold.jsonl" || fail "the keyframe request did not reach the phone"
record '{"step":"input-reaches-phone","actions":["tap","swipe","scroll","text","back"],"out_of_range":"rejected"}'
STOPS=$(grep -c '"event":"mirror-stop"' "$OUT/hold.jsonl" || true)
pkill -f -- "umbriel-link-mirror $ID --frames" || true
wait "$VIEWER" 2> /dev/null || true
wait_for 10 "closing the second viewer did not stop the phone" \
  eval '(( $(grep -c "\"event\":\"mirror-stop\"" "$OUT/hold.jsonl") > STOPS ))'

# The window itself, when gtk4paintablesink is installed: a headless Umbriel in this namespace, the viewer in it, and a
# screenshot that must show the bars.
if gst-inspect-1.0 gtk4paintablesink > /dev/null 2>&1; then
  WL=$RUNTIME/wl
  mkdir -p "$WL"
  chmod 700 "$WL"
  printf '[general]\nautostart = []\nshow_cheatsheet = false\n' > "$WL/umbriel.toml"
  env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$WL" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS=1 "${UMBRIEL:-$ROOT/compositor/build-debug/umbriel}" -c "$WL/umbriel.toml" \
    > "$OUT/umbriel.log" 2>&1 &
  wait_for 10 "umbriel did not start" test -S "$WL/wayland-0"
  in_session() { env -u DISPLAY XDG_RUNTIME_DIR="$WL" WAYLAND_DISPLAY=wayland-0 "$@"; }
  STOPS=$(grep -c '"event":"mirror-stop"' "$OUT/hold.jsonl" || true)
  in_session "$BIN/umbriel-link-mirror" "$ID" --name Phone >> "$OUT/viewer.log" 2>&1 &
  WINDOW=$!
  sleep 4 # real time: the window maps, the decoder starts, and a keyframe arrives
  in_session grim "$OUT/window.png"
  pkill -f -- "umbriel-link-mirror $ID --name" || true
  wait "$WINDOW" 2> /dev/null || true
  python3 - "$OUT/window.png" <<'PY' || fail "the window does not show the bars"
import subprocess, sys
raw = subprocess.run(["magick", sys.argv[1], "-depth", "8", "rgb:-"], capture_output=True, check=True).stdout
pixels = [raw[i:i + 3] for i in range(0, len(raw), 3)]
yellow = sum(1 for r, g, b in pixels if r > 180 and g > 180 and b < 60)
blue = sum(1 for r, g, b in pixels if b > 180 and r < 60 and g < 60)
print(yellow, blue)
assert yellow > 2000 and blue > 2000, (yellow, blue)
PY
  wait_for 10 "closing the window did not stop the phone" \
    eval '(( $(grep -c "\"event\":\"mirror-stop\"" "$OUT/hold.jsonl") > STOPS ))'
  DECODER=$(grep -o 'decoding with [a-z0-9_]*' "$OUT/viewer.log" | tail -1 | cut -d' ' -f3)
  record "{\"step\":\"window-shows-bars\",\"decoder\":\"$DECODER\",\"screenshot\":\"window.png\"}"
fi

cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
