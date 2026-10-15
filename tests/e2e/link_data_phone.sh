#!/usr/bin/env bash
# The phone's data on a real phone, paired with a private umbriel-linkd that it reaches through a UDP relay on a port
# the firewall lets in. Proves:
# 1. DeviceStatus carries the cellular signal level, 0 to 4, while the phone has a SIM in service.
# 2. With the browse and messages switches off (as after pairing), browsing refuses and Contacts stays empty.
# 3. With them on: Contacts holds every contact with a number the phone has; a contact added on the phone arrives
#    through contacts-changed and goes when it is deleted.
# 4. Photos lists the media store by album with only the media permissions (no All files access): a PNG pushed into
#    Pictures/LinkE2E shows in the album, StatFile finds it, ReadFile returns it byte for byte, and Thumbnail is a PNG.
# Needs ANDROID_SERIAL, the debug APK, and the phone on the same network as this machine. Clears Link's data on the
# phone first, so the phone forgets its desktops. Holds /tmp/umbriel-link-phone.lock, as other phone runs do. Writes
# steps.txt, signals.txt, phone.log, linkd.log, and the pulled files to $OUT (default ./artifacts/link-data/phone).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-data/phone}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
PACKAGE=org.umbriel.link
RELAY_PORT=${RELAY_PORT:-57051}

[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-data-phone.XXXX)
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
for permission in READ_CONTACTS READ_MEDIA_IMAGES READ_PHONE_STATE; do
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

signal_level() { prop DeviceStatus | grep -o "'$ID': ([^)]*)" | grep -o -- '-\?[0-9]*)$' | tr -d ')'; }
wait_for 20 "DeviceStatus never showed the phone: $(prop DeviceStatus)" eval '[[ -n $(signal_level) ]]'
LEVEL=$(signal_level)
(( LEVEL >= 0 && LEVEL <= 4 )) || fail "a phone with a SIM in service reported signal $LEVEL: $(prop DeviceStatus)"
step "status: $(prop DeviceStatus | grep -o "'$ID': ([^)]*)"); signal $LEVEL of 4"

REFUSED=$(dbus ListFiles '(ss)' '(a(sbtt))' - "$ID" /)
[[ $REFUSED == *"error"* && $REFUSED == *"llow"* ]] || fail "browsing with the switch off: $REFUSED"
[[ $(prop Contacts) == "(<@a{sa(sas)} {}>,)" ]] || fail "contacts with the switch off: $(prop Contacts)"
step "switches off: browsing refused ($REFUSED), no contacts"

adb shell am force-stop "$PACKAGE"
adb exec-out run-as "$PACKAGE" cat files/devices.json > "$RUNTIME/devices.json"
python3 - "$RUNTIME/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
grants = store["peers"][0].setdefault("grants", {})
grants["browse"] = grants["messages"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
adb shell run-as "$PACKAGE" sh -c "'cat > files/devices.json'" < "$RUNTIME/devices.json"
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
wait_for 20 "the phone did not reconnect: $(devices)" connected

phone_contacts() {
  adb shell content query --uri content://com.android.contacts/contacts --projection _id:display_name \
    --where has_phone_number=1 | grep -c '^Row' || true
}
desktop_contacts() {
  prop Contacts | python3 -c '
import ast, sys
text = sys.stdin.read().strip().replace("@a{sa(sas)} ", "")
value = ast.literal_eval(text[2:-3]) if text.startswith("(<") else {}
print(len(value.get(sys.argv[1], [])))' "$ID"
}
has_contact() { prop Contacts | grep -qF "$1"; }
EXPECTED=$(phone_contacts)
wait_for 30 "Contacts held $(desktop_contacts) of the phone's $EXPECTED" eval '[[ $(desktop_contacts) == "$EXPECTED" ]]'
step "switches on: Contacts holds all $EXPECTED of the phone's contacts with a number"

NAME=LinkE2E$T0
adb shell content insert --uri content://com.android.contacts/raw_contacts --bind account_type:n: --bind account_name:n:
RAW=$(adb shell content query --uri content://com.android.contacts/raw_contacts --projection _id --sort "'_id DESC'" \
  | head -1 | grep -o '_id=[0-9]*' | cut -d= -f2)
adb shell content insert --uri content://com.android.contacts/data --bind raw_contact_id:i:"$RAW" \
  --bind mimetype:s:vnd.android.cursor.item/name --bind data1:s:"$NAME"
adb shell content insert --uri content://com.android.contacts/data --bind raw_contact_id:i:"$RAW" \
  --bind mimetype:s:vnd.android.cursor.item/phone_v2 --bind data1:s:+9779800012345 --bind data2:i:2
ADDED=$(date +%s.%N)
wait_for 30 "the added contact never reached Contacts" has_contact "('$NAME', ['+9779800012345'])"
step "added $NAME on the phone: in Contacts after $(python3 -c "import time,sys; print(round(time.time()-float(sys.argv[1]),1))" "$ADDED") s"
adb shell content delete --uri content://com.android.contacts/raw_contacts --where "_id=$RAW"
wait_for 30 "the deleted contact stayed in Contacts" eval "! has_contact $NAME"
step "deleted it on the phone: gone from Contacts"

SHOT=e2e_$T0.png
python3 - "$OUT/source.png" <<'PY'
import struct, sys, zlib
w, h = 320, 200
raw = b"".join(b"\0" + bytes(v for x in range(w) for v in (x * 255 // w, y * 255 // h, 128)) for y in range(h))
chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY
adb shell mkdir -p /sdcard/Pictures/LinkE2E
adb push "$OUT/source.png" "/sdcard/Pictures/LinkE2E/$SHOT" > /dev/null
adb shell content call --method scan_volume --uri content://media --arg external_primary > /dev/null 2>&1 \
  || adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "file:///sdcard/Pictures/LinkE2E/$SHOT" > /dev/null
in_album() { dbus ListFiles '(ss)' '(a(sbtt))' - "$ID" /Photos/LinkE2E | grep -q "$SHOT"; }
wait_for 30 "the album never listed $SHOT: $(dbus ListFiles '(ss)' '(a(sbtt))' - "$ID" /Photos)" in_album
ROOTS=$(dbus ListFiles '(ss)' '(a(sbtt))' - "$ID" /)
[[ $ROOTS == *"'Photos'"* && $ROOTS != *"'Storage'"* ]] || fail "roots without All files access: $ROOTS"
step "roots without All files access: $ROOTS; Photos/LinkE2E lists $SHOT"
STAT=$(dbus StatFile '(ss)' '(sbtt)' - "$ID" "/Photos/LinkE2E/$SHOT")
[[ $STAT == *"'$SHOT'"* ]] || fail "StatFile: $STAT"
SIZE=$(stat -c %s "$OUT/source.png")
dbus ReadFile '(sstu)' '(ay)' "$OUT/pulled.png" "$ID" "/Photos/LinkE2E/$SHOT" 0 "$SIZE" > /dev/null
cmp -s "$OUT/source.png" "$OUT/pulled.png" || fail "ReadFile returned other bytes"
THUMB=$(dbus Thumbnail '(ssq)' '(ay)' "$OUT/thumbnail.png" "$ID" "/Photos/LinkE2E/$SHOT" 256)
[[ $(head -c 8 "$OUT/thumbnail.png" | xxd -p) == 89504e470d0a1a0a ]] || fail "Thumbnail is not a PNG: $THUMB"
step "StatFile found it; ReadFile identical ($SIZE bytes); Thumbnail a PNG of $THUMB bytes"
adb shell rm -r /sdcard/Pictures/LinkE2E
adb shell content call --method scan_volume --uri content://media --arg external_primary > /dev/null 2>&1 || true

adb logcat -d > "$OUT/phone.log"
! grep -q "FATAL EXCEPTION" "$OUT/phone.log" || fail "the app crashed; see phone.log"
step "no crash"
echo "PASS; artifacts: $OUT"
