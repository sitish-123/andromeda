#!/usr/bin/env bash
# Texts on a real phone, paired with a private umbriel-linkd that it reaches through a UDP relay on a port the firewall
# lets in. Proves, without sending a text:
# 1. With the messages switch off (as after pairing), Texts stays empty and SendText is refused (not-allowed).
# 2. With it on, D-Bus Texts holds the phone's newest texts (up to 256), the same ids in the same order as the SMS
#    provider lists them, each marked sent or received as the provider says.
# Bodies and numbers stay out of the artifacts; steps.txt records counts and ids only. Needs ANDROID_SERIAL, the debug
# APK, and the phone on the same network as this machine. Clears Link's data on the phone first, so the phone forgets its
# desktops. Holds /tmp/umbriel-link-phone.lock, as other phone runs do. Writes steps.txt and linkd.log to $OUT (default
# ./artifacts/link-texts-phone).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-texts-phone}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
PACKAGE=org.umbriel.link
RELAY_PORT=${RELAY_PORT:-57051}

[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-texts-phone.XXXX)
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
for permission in READ_SMS SEND_SMS READ_CONTACTS; do
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
T0=$(date +%s)
adb logcat -c
prop() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 "$1"; }
dbus() { python3 - "$@" <<'PY'
import sys
from gi.repository import Gio, GLib
method, signature, reply, out, *args = sys.argv[1:]
args = [int(a) if c in "qut" else a for a, c in zip(args, signature.strip("()"))]
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
try:
    result = bus.call_sync("org.umbriel.Link1", "/org/umbriel/Link1", "org.umbriel.Link1", method,
                           GLib.Variant(signature, tuple(args)), GLib.VariantType(reply), Gio.DBusCallFlags.NONE,
                           30000, None).unpack()
except GLib.Error as error:
    print(f"error {error.message}")
    sys.exit(0)
if out != "-":
    open(out, "wb").write(bytes(result[0]))
    print(len(result[0]))
else:
    print(repr(result[0]))
PY
}


REFUSED=$(dbus SendText '(sss)' '()' - "$ID" +9779800000000 hello)
[[ $REFUSED == *"not-allowed"* ]] || fail "SendText with the switch off: $REFUSED"
[[ $(prop Texts) == "(<@a{sa(ttsstb)} {}>,)" ]] || fail "texts with the switch off"
step "switch off: no texts; SendText refused (not-allowed), nothing sent"

adb shell am force-stop "$PACKAGE"
adb exec-out run-as "$PACKAGE" cat files/devices.json > "$RUNTIME/devices.json"
python3 - "$RUNTIME/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["messages"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
adb shell run-as "$PACKAGE" sh -c "'cat > files/devices.json'" < "$RUNTIME/devices.json"
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
wait_for 20 "the phone did not reconnect: $(devices)" connected

# The provider's newest 256 as "id:sent" lines, sent being any type but inbox (1).
phone_texts() {
  adb shell content query --uri content://sms --projection _id:type --sort "'date DESC, _id DESC'" \
    | head -256 | sed -E 's/.*_id=([0-9]+), type=([0-9]+).*/\1 \2/' | awk '{print $1 ":" ($2 == 1 ? "False" : "True")}'
}
desktop_texts() {
  python3 - "$ID" <<'PY'
import sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
value = bus.call_sync("org.umbriel.Link1", "/org/umbriel/Link1", "org.freedesktop.DBus.Properties", "Get",
                      GLib.Variant("(ss)", ("org.umbriel.Link1", "Texts")), None, Gio.DBusCallFlags.NONE, 5000,
                      None).unpack()[0]
for text in value.get(sys.argv[1], []):
    print(f"{text[0]}:{text[5]}")
PY
}
phone_texts > "$RUNTIME/phone-texts.txt"
EXPECTED=$(wc -l < "$RUNTIME/phone-texts.txt")
(( EXPECTED > 0 )) || fail "the phone has no texts to compare"
same() { desktop_texts > "$RUNTIME/desktop-texts.txt"; cmp -s "$RUNTIME/phone-texts.txt" "$RUNTIME/desktop-texts.txt"; }
wait_for 60 "Texts differ from the provider" same
SENT=$(grep -c ':True$' "$RUNTIME/phone-texts.txt" || true)
step "switch on: Texts holds the provider's newest $EXPECTED ($SENT sent), same ids and order; newest id $(head -1 "$RUNTIME/phone-texts.txt" | cut -d: -f1)"

! adb logcat -d | grep -q "FATAL EXCEPTION" || fail "the app crashed"
step "no crash"
echo "PASS; artifacts: $OUT"
