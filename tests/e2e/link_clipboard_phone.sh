#!/usr/bin/env bash
# Phone-to-PC clipboard through the real watcher on a real phone, paired with a private umbriel-linkd that it reaches
# through a UDP relay on a port the firewall lets in. Proves:
# 1. Started in the background (as after a boot or an update), the watcher starts no logcat: Android would refuse it
#    without a prompt and keep refusing for a minute, which also refused the foreground start that followed.
# 2. So before the first open a copy does not cross; the first open asks for log access once.
# 3. With the app in the background, another app's PNG copy (a pipe-backed content URI) and text copy each reach the
#    desktop as a ClipboardOffered and pull back byte for byte; nothing tries a background activity start, which
#    Android 15 and later block even with "Display over other apps".
# 4. A second open asks nothing and keeps the same logcat, and a further copy still crosses.
# 5. A screenshot (a PNG in Pictures/Screenshots) shared to Link by its media store URI arrives as a file, identical;
#    OxygenOS screenshots do not touch the clipboard, so Share (or Copy in the screenshot editor) is how they cross.
# The desktop half (the shell taking the selection, wl-paste) is link_clipboard.sh. Needs ANDROID_SERIAL, the debug
# APKs, and the phone on the same network as this machine. Clears Link's data on the phone first, so the phone forgets
# its desktops. Holds /tmp/umbriel-link-phone.lock, as other phone runs do. Writes steps.txt, signals.txt, phone.log,
# linkd.log, and screenshots to $OUT (default ./artifacts/link-clipboard/phone).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-clipboard/phone}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
FIXTURE_APK=${FIXTURE_APK:-$ROOT/link/android/fixture/build/outputs/apk/debug/fixture-debug.apk}
PACKAGE=org.umbriel.link
FIXTURE=org.umbriel.link.fixture
RELAY_PORT=${RELAY_PORT:-57051}

[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-clipboard-phone.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
step() { printf '%s\n' "$*" | tee -a "$OUT/steps.txt"; }
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}
adb() { command adb -s "$SERIAL" "$@"; }

[[ $(adb get-state 2>/dev/null) == device ]] || fail "no phone at $SERIAL"
[[ -s $APK && -s $FIXTURE_APK ]] || fail "no APKs at $APK and $FIXTURE_APK"

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
offers() { grep -c "member=ClipboardOffered" "$OUT/signals.txt" || true; }
last_clip() { grep -A2 'member=ClipboardOffered' "$OUT/signals.txt" | grep -o 'uint64 [0-9]*' | tail -1 | cut -d' ' -f2; }
pull() { python3 "$ROOT/tests/e2e/link_clipboard_dbus.py" pull "$ID" "$(last_clip)" "$1"; }

XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$RUNTIME/downloads RUST_LOG=info,zbus=warn "$BIN/umbriel-linkd" \
  > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
PORT=$(grep -o 'listening on port [0-9]*' "$OUT/linkd.log" | grep -o '[0-9]*$')
HOST=$(ip -4 route get 1.1.1.1 | grep -o 'src [0-9.]*' | cut -d' ' -f2)
# The daemon's own port is random and firewalled; the phone dials the relay, which the daemon sees as 127.0.0.1.
python3 - "$RELAY_PORT" "$PORT" <<'PY' &
import selectors, socket, sys
listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
listen.bind(("0.0.0.0", int(sys.argv[1])))
target, clients, peers = ("127.0.0.1", int(sys.argv[2])), {}, {}
sel = selectors.DefaultSelector()
sel.register(listen, selectors.EVENT_READ)
while True:
    for key, _ in sel.select():
        data, source = key.fileobj.recvfrom(65535)
        if key.fileobj is listen:
            if source not in clients:
                upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                upstream.connect(target)
                clients[source], peers[upstream] = upstream, source
                sel.register(upstream, selectors.EVENT_READ)
            clients[source].send(data)
        else:
            listen.sendto(data, peers[key.fileobj])
PY

adb shell input keyevent KEYCODE_WAKEUP
adb shell cmd statusbar collapse
# Never uninstall: other phone runs share the app and its signing key. Clearing its data forgets other desktops.
adb shell pm clear "$PACKAGE" > /dev/null 2>&1 || true
adb install -r -g "$APK" > /dev/null || fail "installing $APK"
adb install -r "$FIXTURE_APK" > /dev/null || fail "installing $FIXTURE_APK"
for permission in READ_LOGS SYSTEM_ALERT_WINDOW; do
  adb shell dumpsys package "$PACKAGE" | grep -q "$permission: granted=true" || fail "$permission was not granted at install"
done

ui() { adb shell uiautomator dump /sdcard/link-ui.xml > /dev/null 2>&1 || true; adb shell cat /sdcard/link-ui.xml > "$RUNTIME/ui.xml" || true; }
bounds() {
  python3 - "$RUNTIME/ui.xml" "$1" <<'PY'
import re, sys
xml, want = open(sys.argv[1]).read(), sys.argv[2].lower()
for node in re.finditer(r'<node [^>]*>', xml):
    attrs = dict(re.findall(r'(\w[\w-]*)="([^"]*)"', node.group(0)))
    if want in (attrs.get("text", "").lower(), attrs.get("content-desc", "").lower()):
        l, t, r, b = map(int, re.findall(r"\d+", attrs["bounds"]))
        print((l + r) // 2, (t + b) // 2)
        break
PY
}
tap_text() {
  local shot=$1; shift
  for _ in $(seq 40); do
    ui
    for text in "$@"; do
      local at
      at=$(bounds "$text")
      if [[ -n $at ]]; then
        [[ -z $shot ]] || adb exec-out screencap -p > "$OUT/$shot.png"
        adb shell input tap $at
        return
      fi
    done
    sleep 0.5
  done
  fail "none of $* appeared on screen"
}

URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/" | sed -E "s/&a=[^&]*//g; s/&b=[^&]*//")
adb shell am start -W -a android.intent.action.VIEW -d "'$URI&a=$HOST:$RELAY_PORT'" "$PACKAGE" > /dev/null
tap_text "" Pair
wait_for 20 "D-Bus does not show the phone connected: $(devices)" connected
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
step "paired with a private desktop through the relay on $HOST:$RELAY_PORT"

# Pairing opens the app, whose watcher asks for log access in the foreground. That prompt would outlive the process
# killed below and decline the next request; allow it so it closes, then go on with a fresh process.
for _ in $(seq 10); do
  if adb shell dumpsys activity activities | grep -q LogAccessDialogActivity; then
    tap_text "" "Allow one-time access"
    break
  fi
  sleep 0.5
done

watcher() { adb shell ps -A -o PID,PPID,NAME | awk -v app="$(adb shell pidof $PACKAGE | tr -d '\r')" '$2 == app && $3 == "logcat" {print $1}'; }
prompts() { adb logcat -d -b events | grep -c "wm_create_activity.*LogAccessDialogActivity" || true; }
background() { adb shell input keyevent KEYCODE_HOME; sleep 1; }
copy_text() { adb shell am start -W -n "$FIXTURE/.TextClipboardActivity" --es text "'$1'" > /dev/null; }
copy_image() { adb shell am start -W -n "$FIXTURE/.ImageClipboardActivity" > /dev/null; }

# Stay connected, then an update, which starts the app's process in the background. A force-stop would leave the app
# stopped, and ColorOS then drops MY_PACKAGE_REPLACED; killing the cached process does not.
adb shell input keyevent KEYCODE_HOME
sleep 1
printf '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n<map><boolean name="stay_connected" value="true" /></map>\n' \
  | adb shell run-as "$PACKAGE" sh -c "'mkdir -p shared_prefs && cat > shared_prefs/presence.xml'"
adb shell am kill "$PACKAGE"
adb logcat -c
adb logcat -c -b events
SESSIONS=$(grep -c "connected over IP" "$OUT/linkd.log")
# An update: MY_PACKAGE_REPLACED starts Stay connected from the background.
adb install -r "$APK" > /dev/null || fail "reinstalling $APK"
wait_for 30 "the app did not connect from the background" eval '(( $(grep -c "connected over IP" "$OUT/linkd.log") > SESSIONS ))'
sleep 2 # real time: the watcher would have started its logcat by now
[[ -z $(watcher) ]] || fail "the watcher started a logcat in the background"
step "background start: connected, no logcat"

BEFORE=$(offers)
copy_text "before the first open"
sleep 4 # real time: an offer would arrive within this
[[ $(offers) == "$BEFORE" ]] || fail "a copy crossed before log access was asked"
step "before the first open: a copy does not cross"

adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
tap_text 1-log-access-prompt "Allow one-time access"
wait_for 10 "the dialog did not close" eval '! adb shell dumpsys window | grep -q "mCurrentFocus.*LogAccessDialog"'
wait_for 10 "the watcher has no logcat after the first open" eval '[[ -n $(watcher) ]]'
WATCHER=$(watcher)
[[ $(prompts) == 1 ]] || fail "$(prompts) log-access prompts on the first open"
step "first open: one log-access prompt, allowed; watcher logcat pid $WATCHER"

background
BEFORE=$(offers)
T0=$(date +%s%N)
copy_image
wait_for 20 "the phone's PNG copy never reached the desktop" eval '(( $(offers) > BEFORE ))'
MS=$(( ($(date +%s%N) - T0) / 1000000 ))
grep -A4 "member=ClipboardOffered" "$OUT/signals.txt" | tail -4 | grep -q '"image/png"' || fail "the offer is not image/png"
adb exec-out run-as "$FIXTURE" cat files/clipboard.png > "$OUT/source.png"
adb shell run-as "$FIXTURE" rm files/clipboard.png
pull image/png > "$OUT/pulled.png" || fail "PullClipboard for the PNG"
cmp -s "$OUT/source.png" "$OUT/pulled.png" || fail "the pulled PNG differs"
step "background PNG copy: ClipboardOffered image/png ${MS} ms after the copy started, pulled identical after the source was deleted ($(stat -c %s "$OUT/pulled.png") bytes)"

BEFORE=$(offers)
copy_text "copied on the phone $T0"
wait_for 20 "the phone's text copy never reached the desktop" eval '(( $(offers) > BEFORE ))'
PULLED=$(pull "text/plain;charset=utf-8") || fail "PullClipboard for the text"
[[ $PULLED == "copied on the phone $T0" ]] || fail "the text arrived as \"$PULLED\""
step "background text copy: pulled identical"

adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
sleep 3 # real time: a second prompt would show by now
adb exec-out screencap -p > "$OUT/2-second-open.png"
background
[[ $(prompts) == 1 ]] || fail "the second open asked for log access again"
[[ $(watcher) == "$WATCHER" ]] || fail "the second open replaced the watcher's logcat"
BEFORE=$(offers)
copy_image
wait_for 20 "a PNG copy after the second open never reached the desktop" eval '(( $(offers) > BEFORE ))'
step "second open: no prompt, same logcat pid $WATCHER; a further PNG copy crosses"

# A screenshot reaches the desktop through its Share: a PNG in Pictures/Screenshots, shared by its media store URI.
SHOT=Screenshot_e2e_$T0.png
adb exec-out run-as "$FIXTURE" cat files/clipboard.png > "$RUNTIME/$SHOT"
adb push "$RUNTIME/$SHOT" "/sdcard/Pictures/Screenshots/$SHOT" > /dev/null
adb shell content call --uri content://media --method scan_volume --arg external_primary > /dev/null
MEDIA=$(adb shell content query --uri content://media/external/images/media --projection _id \
  --where "\"_display_name='$SHOT'\"" | grep -o '_id=[0-9]*' | cut -d= -f2 | tr -d '\r')
[[ -n $MEDIA ]] || fail "the screenshot is not in the media store"
OFFERS=$(grep -c 'member=TransferOffered' "$OUT/signals.txt" || true)
adb shell am start -W -n "$PACKAGE/.share.ShareActivity" -a android.intent.action.SEND -t image/png \
  --eu android.intent.extra.STREAM "content://media/external/images/media/$MEDIA" > /dev/null
wait_for 30 "no TransferOffered for the shared screenshot" eval '(( $(grep -c "member=TransferOffered" "$OUT/signals.txt") > OFFERS ))'
OFFER=$(grep -A1 'member=TransferOffered' "$OUT/signals.txt" | grep -o '"[0-9a-f]\{32\}"' | tail -1 | tr -d '"')
link AcceptTransfer "'$OFFER'" > /dev/null || fail "AcceptTransfer for the screenshot"
wait_for 30 "the screenshot never arrived" test -f "$RUNTIME/downloads/$SHOT"
cmp -s "$RUNTIME/$SHOT" "$RUNTIME/downloads/$SHOT" || fail "the shared screenshot differs"
adb shell rm "/sdcard/Pictures/Screenshots/$SHOT"
adb shell content call --uri content://media --method scan_volume --arg external_primary > /dev/null
step "screenshot shared from Pictures/Screenshots: offered, accepted, identical"

adb logcat -d | grep -E "Denying clipboard access to $PACKAGE|callingPackage=$PACKAGE\$|ClipboardReadActivity|BAL_BLOCK|FATAL|AndroidRuntime: [^>]|W link" \
  > "$OUT/phone.log" || true
! grep -q "BAL_BLOCK" "$OUT/phone.log" || fail "a background activity start was blocked"
! grep -q "FATAL EXCEPTION" "$OUT/phone.log" || fail "the app crashed"
step "no blocked activity start, no crash"
echo "PASS; artifacts: $OUT"
