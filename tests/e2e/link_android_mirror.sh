#!/usr/bin/env bash
# Mirroring the emulator's screen with the real app: MediaProjection into the hardware (here software) H.264 encoder,
# streamed to a private umbriel-linkd and decoded by umbriel-link-mirror --frames; input through the app's
# accessibility service. Android's own prompts are answered through uiautomator, as a user taps them.
#
# Proves:
# 1. With the app's Screen switch off, as after pairing, OpenMirror is refused.
# 2. With it on, the request is a notification; Allow, then Android's capture prompt (entire screen), starts the stream;
#    the viewer decodes 90 frames at the capture size (at most 1080 on the short side, multiples of 16) showing the
#    emulator's screen.
# 3. Input from the desktop acts on the phone: Home leaves Settings; a tap on Settings' search opens it; text types
#    into it; Back closes it.
# 4. Closing the viewer stops the capture service on the phone.
# Needs the debug APK and a running emulator. Writes results.jsonl, frames/, *.png, ui-*.xml, linkd.log, viewer.log
# to $OUT (default ./artifacts/link-android-mirror).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-android-mirror}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
MAESTRO=${MAESTRO:-$HOME/.maestro/bin/maestro}
SERIAL=${ANDROID_SERIAL:-emulator-5554}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
FLOWS=$ROOT/link/android/maestro
PACKAGE=org.umbriel.link

rm -rf "$OUT"
mkdir -p "$OUT"
# A fresh directory per run: dbus-daemon unlinks its socket path on exit, so a reused path would race the old one.
RUNTIME=$(mktemp -d /tmp/link-android-mirror.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}
adb() { "$ADB" -s "$SERIAL" "$@"; }
maestro() { "$MAESTRO" --device "$SERIAL" test -e OUT="$OUT" "$@" >> "$OUT/maestro.log" 2>&1 || fail "maestro $*"; }
screenshot() { adb exec-out screencap -p > "$OUT/$1.png"; }
notifications() { adb shell dumpsys notification --noredact > "$OUT/notifications.txt"; cat "$OUT/notifications.txt"; }

[[ $(adb get-state 2>/dev/null) == device ]] || fail "no emulator at $SERIAL"
[[ -s $APK ]] || fail "no APK at $APK"

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
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
connected() { devices | grep -q "', true)"; }
disconnected() { devices | grep -q "', false)"; }
received() { grep -qF "string \"$1\"" "$OUT/signals.txt"; }
posted() { notifications | grep -qF "$1"; }
gone() { ! notifications | grep -qF "$1"; }

DOWNLOADS=$RUNTIME/downloads
mkdir -p "$DOWNLOADS"
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"

adb shell cmd statusbar collapse
adb shell input keyevent KEYCODE_HOME
adb uninstall "$PACKAGE" > /dev/null 2>&1 || true
adb install "$APK" > /dev/null || fail "installing $APK"
record '{"step":"installed"}'

URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
[[ $URI == umbriel-link:pair\?* ]] || fail "no pairing URI"
adb shell am start -W -a android.intent.action.VIEW -d "'$URI'" "$PACKAGE" > /dev/null
maestro "$FLOWS/pair.yaml"
wait_for 20 "D-Bus does not show the emulator connected: $(devices)" connected
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
NAME=$(devices | sed -E "s/.*'[0-9a-f]{32}', '([^']*)'.*/\1/")
record "{\"step\":\"paired-and-connected\",\"device\":\"$ID\",\"name\":\"$NAME\"}"


ui() { adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1; adb shell cat /sdcard/ui.xml > "$OUT/ui-$1.xml"; }
# bounds NAME TEXT: the centre of the first node whose text or description is TEXT (any case), as "x y", from dump NAME;
# a prefix match with a trailing '*'.
bounds() {
  python3 - "$OUT/ui-$1.xml" "$2" <<'PY'
import re, sys
xml, want = open(sys.argv[1]).read(), sys.argv[2].lower()
for node in re.finditer(r'<node [^>]*>', xml):
    attrs = dict(re.findall(r'(\w[\w-]*)="([^"]*)"', node.group(0)))
    labels = [attrs.get("text", "").lower(), attrs.get("content-desc", "").lower()]
    if any(label.startswith(want[:-1]) if want.endswith("*") else label == want for label in labels):
        l, t, r, b = map(int, re.findall(r"\d+", attrs["bounds"]))
        print((l + r) // 2, (t + b) // 2)
        break
PY
}
# tap_text NAME TEXT...: wait for any of TEXT on screen and tap it.
tap_text() {
  local name=$1; shift
  for _ in $(seq 40); do
    ui "$name"
    for text in "$@"; do
      local at
      at=$(bounds "$name" "$text")
      if [[ -n $at ]]; then adb shell input tap $at; return; fi
    done
    sleep 0.5
  done
  fail "none of $* appeared on screen"
}
focus() { adb shell dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity'; }
# allow_capture NAME: Start on Android's capture prompt, which the app opens for the whole screen.
allow_capture() {
  tap_text "$1" Start "Start now"
}
screen_size() { adb shell wm size | grep -o '[0-9]*x[0-9]*' | tail -1; }

REFUSED=$(link OpenMirror "'$ID'" 2>&1 || true)
[[ $REFUSED == *Refused*"screen switch"* ]] || fail "OpenMirror with the Screen switch off: $REFUSED"
record '{"step":"refused-with-switch-off"}'

SESSIONS=$(grep -c "connected over IP" "$OUT/linkd.log")
adb shell am force-stop "$PACKAGE"
adb shell run-as "$PACKAGE" cat files/devices.json > "$RUNTIME/phone.json"
python3 - "$RUNTIME/phone.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["screen"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
adb shell run-as "$PACKAGE" sh -c "'cat > files/devices.json'" < "$RUNTIME/phone.json"
adb shell settings put secure enabled_accessibility_services "$PACKAGE/$PACKAGE.screen.ScreenInput"
adb shell settings put secure accessibility_enabled 1
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
# The desktop lists the old session as connected until it times out; only a new one counts.
wait_for 30 "the app did not reconnect" eval '(( $(grep -c "connected over IP" "$OUT/linkd.log") > SESSIONS ))'
record '{"step":"screen-switch-and-control-on"}'

"$BIN/umbriel-link-mirror" "$ID" --frames "$OUT/frames" --count 90 > "$OUT/viewer.log" 2>&1 &
VIEWER=$!
wait_for 15 "no mirror request notification" eval 'adb shell dumpsys notification --noredact | grep -q "wants to show your screen"'
adb shell cmd statusbar expand-notifications
tap_text notification Allow
allow_capture prompt
wait "$VIEWER" || fail "the viewer failed: $(tail -3 "$OUT/viewer.log")"
FRAMES=$(ls "$OUT/frames" | wc -l)
(( FRAMES >= 85 )) || fail "only $FRAMES frames decoded"
SIZE=$(magick identify -format '%wx%h' "$OUT/frames/frame-060.png")
W=${SIZE%x*} H=${SIZE#*x}
(( W % 16 == 0 && H % 16 == 0 && (W < H ? W : H) <= 1080 )) || fail "capture size $SIZE breaks the rules"
COLOURS=$(magick "$OUT/frames/frame-060.png" -format '%k' info:)
(( COLOURS > 100 )) || fail "the frames are blank ($COLOURS colours)"
record "{\"step\":\"streams-the-screen\",\"frames\":$FRAMES,\"size\":\"$SIZE\",\"screen\":\"$(screen_size)\",\"colours\":$COLOURS}"

"$BIN/umbriel-link-mirror" "$ID" --frames "$OUT/frames-input" --count 100000 >> "$OUT/viewer.log" 2>&1 &
VIEWER=$!
wait_for 15 "no second request" eval 'adb shell dumpsys notification --noredact | grep -q "wants to show your screen"'
adb shell cmd statusbar expand-notifications
tap_text notification2 Allow
allow_capture prompt2
wait_for 15 "the second mirror did not start" eval '[[ $(grep -c "mirror\] .*mirroring at" "$OUT/linkd.log") -ge 2 ]]'
adb shell cmd statusbar collapse

adb shell am start -W -a android.settings.SETTINGS > /dev/null
wait_for 10 "Settings did not open" eval 'focus | grep -q settings'
link MirrorInput "'$ID'" home "{}" > /dev/null
wait_for 10 "Home from the desktop did not leave Settings: $(focus)" eval '! focus | grep -q settings'
record '{"step":"home-from-desktop"}'

adb shell am start -W -a android.settings.SETTINGS > /dev/null
wait_for 10 "Settings did not reopen" eval 'focus | grep -q settings'
sleep 1
ui settings
read -r SX SY <<< "$(bounds settings "Search settings")"
[[ -n $SX ]] || fail "no search field in Settings"
read -r PW PH <<< "$(screen_size | tr x ' ')"
FX=$(( SX * 10000 / PW )) FY=$(( SY * 10000 / PH ))
link MirrorInput "'$ID'" tap "{'x': <int32 $FX>, 'y': <int32 $FY>}" > /dev/null
wait_for 10 "the tap did not open search: $(focus)" eval 'focus | grep -qi search'
sleep 1
link MirrorInput "'$ID'" text "{'text': <'wifi'>}" > /dev/null
wait_for 10 "the text did not arrive" eval 'ui typed; grep -q "text=\"wifi\"" "$OUT/ui-typed.xml"'
link MirrorInput "'$ID'" back "{}" > /dev/null
link MirrorInput "'$ID'" back "{}" > /dev/null
wait_for 10 "Back from the desktop did not close search: $(focus)" eval '! focus | grep -qi search'
record "{\"step\":\"input-acts-on-phone\",\"tap\":[$FX,$FY],\"actions\":[\"home\",\"tap\",\"text\",\"back\"]}"

kill "$VIEWER"
wait "$VIEWER" 2> /dev/null || true
wait_for 10 "closing the viewer left the capture running" \
  eval '! adb shell dumpsys activity services "$PACKAGE" | grep -q ScreenCapture'
record '{"step":"viewer-close-stops-capture"}'
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
