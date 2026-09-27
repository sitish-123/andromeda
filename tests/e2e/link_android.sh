#!/usr/bin/env bash
# The Android app on an emulator against a private umbriel-linkd (own dbus-daemon, own XDG_STATE_HOME), reached through
# the emulator's NAT at the host's LAN addresses in the pairing URI. Proves: QR pairing from the opened URI (Maestro
# taps Pair and grants notifications), the app shows Connected and D-Bus Devices agrees; text and a link shared from
# Android through the share target arrive as D-Bus Received; text and a link sent with D-Bus Share post Android
# notifications, and tapping the text's (its Copy PendingIntent) clears it; leaving the app ends presence (D-Bus shows it
# disconnected) and returning restores it; "Stay connected" keeps it connected in the background with its
# foreground-service notification, which goes away when turned off; `dumpsys battery set level 42` shows as 42% in
# D-Bus DeviceStatus; a file shared from the Files app through the share
# target arrives intact after a D-Bus Accept, and a file sent with D-Bus SendFiles and accepted from the Android
# notification lands in MediaStore Downloads, no longer pending, with the same SHA-256; with READ_LOGS and Display over
# other apps granted over adb, a copy in Settings reaches the desktop with no tap, a desktop clip is set on the phone
# (pasted back in Settings) without echoing, and "Send to desktop" (PROCESS_TEXT) sends a selection; unpairing in the
# app removes it on the desktop.
# Writes screenshots, results.jsonl, results.json, signals.txt, notifications.txt, mediastore.txt, linkd.log to $OUT
# (default ./artifacts/link-android). Needs the debug APK (./gradlew :app:assembleDebug) and a running emulator.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-android}
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
RUNTIME=$(mktemp -d /tmp/link-android.XXXX)
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
# The one-time grants that turn on automatic phone-to-desktop clipboard: READ_LOGS (a development permission) and
# Display over other apps.
adb shell pm grant "$PACKAGE" android.permission.READ_LOGS
adb shell appops set "$PACKAGE" SYSTEM_ALERT_WINDOW allow
record '{"step":"installed"}'

URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
[[ $URI == umbriel-link:pair\?* ]] || fail "no pairing URI"
adb shell am start -W -a android.intent.action.VIEW -d "'$URI'" "$PACKAGE" > /dev/null
maestro "$FLOWS/pair.yaml"
wait_for 20 "D-Bus does not show the emulator connected: $(devices)" connected
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
NAME=$(devices | sed -E "s/.*'[0-9a-f]{32}', '([^']*)'.*/\1/")
record "{\"step\":\"paired-and-connected\",\"device\":\"$ID\",\"name\":\"$NAME\"}"

status() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 DeviceStatus; }
wait_for 20 "the app never reported its status: $(status)" eval 'status | grep -q "$ID"'
adb shell dumpsys battery unplug
adb shell dumpsys battery set level 42
wait_for 20 "DeviceStatus never showed 42%: $(status)" eval 'status | grep -qF "(uint32 42, false,"'
adb shell dumpsys battery reset
record "{\"step\":\"battery-status\",\"status\":\"$(status | grep -o "(uint32 [^)]*)")\"}"

adb shell am start -W -n "$PACKAGE/.share.ShareActivity" -a android.intent.action.SEND -t text/plain \
  --es android.intent.extra.TEXT "'hello from android'" > /dev/null
wait_for 20 "no Received for the Android text" received "hello from android"
screenshot 3-sent-text
adb shell am start -W -n "$PACKAGE/.share.ShareActivity" -a android.intent.action.SEND -t text/plain \
  --es android.intent.extra.TEXT "'https://example.org/from-android'" > /dev/null
wait_for 20 "no Received for the Android link" received "https://example.org/from-android"
grep -B1 -F 'string "https://example.org/from-android"' "$OUT/signals.txt" | grep -qF 'string "link"' \
  || fail "the Android link did not arrive as a link"
record '{"step":"android-to-desktop","kinds":["text","link"]}'

link Share "$ID" text "'hello from the desktop'" > /dev/null || fail "D-Bus Share text"
link Share "$ID" link "'https://example.org/from-desktop'" > /dev/null || fail "D-Bus Share link"
wait_for 10 "no Android notification for the text" posted "hello from the desktop"
wait_for 10 "no Android notification for the link" posted "https://example.org/from-desktop"
adb shell cmd statusbar expand-notifications
sleep 2 # real time: the shade animates open and lays out the notifications
screenshot 4-notifications
adb shell cmd statusbar collapse
maestro "$FLOWS/copy.yaml"
wait_for 10 "Copy did not clear the text notification" gone "hello from the desktop"
record '{"step":"desktop-to-android","kinds":["text","link"],"copy":"content intent, the Copy PendingIntent"}'

# A real file from the phone's Downloads through the share target, accepted on D-Bus.
head -c 3145728 /dev/urandom > "$RUNTIME/phone.bin"
adb shell rm -f /sdcard/Download/e2e-phone.bin /sdcard/Download/e2e-desk.bin
media_id() {
  adb shell content query --uri content://media/external/downloads --projection _id \
    --where "\"_display_name='$1'\"" | grep -o '_id=[0-9]*' | head -1 | cut -d= -f2
}
# A Downloads entry, as a browser leaves it, shared from the Files app: the shell cannot grant MediaStore URIs itself.
adb shell content insert --uri content://media/external/downloads --bind _display_name:s:e2e-phone.bin
adb shell content write --uri "content://media/external/downloads/$(media_id e2e-phone.bin)" < "$RUNTIME/phone.bin"
adb shell am start -W -a android.intent.action.VIEW -d content://com.android.providers.downloads.documents/root/downloads \
  com.google.android.documentsui > /dev/null
maestro "$FLOWS/share-file.yaml"
wait_for 30 "no TransferOffered for the phone's file" grep -q "member=TransferOffered" "$OUT/signals.txt"
OFFER=$(grep -A1 'member=TransferOffered' "$OUT/signals.txt" | grep -o '"[0-9a-f]\{32\}"' | head -1 | tr -d '"')
link AcceptTransfer "'$OFFER'" > /dev/null || fail "AcceptTransfer"
wait_for 60 "the phone's file never arrived" test -f "$DOWNLOADS/e2e-phone.bin"
[[ $(sha256sum < "$RUNTIME/phone.bin") == $(sha256sum < "$DOWNLOADS/e2e-phone.bin") ]] || fail "the phone's file differs"
record '{"step":"android-share-target-file","bytes":3145728,"sha256_match":true}'

# A file from the desktop, accepted from the Android notification, published to Downloads once verified.
head -c 2097152 /dev/urandom > "$RUNTIME/e2e-desk.bin"
# Behind the Files app the phone is not present, so its session ends once the transfer does.
wait_for 20 "the phone stayed connected behind the Files app" disconnected
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
wait_for 20 "the app is not connected before the desktop sends" connected
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/e2e-desk.bin" > /dev/null || fail "SendFiles"
maestro "$FLOWS/accept.yaml"
pending() {
  adb shell content query --uri content://media/external/downloads --projection is_pending \
    --where "\"_display_name='e2e-desk.bin'\"" | grep -o 'is_pending=[0-9]' | cut -d= -f2
}
wait_for 60 "the desktop's file never landed in Downloads" eval '[[ $(pending) == 0 ]]'
adb shell content query --uri content://media/external/downloads --projection _display_name:is_pending:_size \
  --where "\"_display_name='e2e-desk.bin'\"" > "$OUT/mediastore.txt"
[[ $(adb shell sha256sum /sdcard/Download/e2e-desk.bin | cut -d' ' -f1) == $(sha256sum < "$RUNTIME/e2e-desk.bin" | cut -d' ' -f1) ]] \
  || fail "the phone's Downloads copy differs"
adb shell rm -f /sdcard/Download/e2e-desk.bin /sdcard/Download/e2e-phone.bin
record '{"step":"desktop-file-to-android-downloads","bytes":2097152,"is_pending":0,"sha256_match":true}'

adb shell input keyevent KEYCODE_HOME
wait_for 15 "presence outlived the app leaving the foreground" disconnected
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
wait_for 20 "presence did not return with the app" connected
record '{"step":"presence-follows-foreground"}'

maestro -e STAY=on "$FLOWS/stay-connected.yaml"
maestro "$FLOWS/allow-logs.yaml"
wait_for 10 "no Stay connected notification" posted "Connected to your desktops"
adb shell input keyevent KEYCODE_HOME
sleep 5 # real time: longer than the foreground check above waited for a disconnect
connected || fail "Stay connected did not keep the session in the background: $(devices)"
screenshot 6-stay-connected-background

# Automatic clipboard both ways while Stay connected runs, in Settings' device-name field: another app's copy.
clip_offers() { grep -c "member=ClipboardOffered" "$OUT/signals.txt" || true; }
COPIED=$(adb shell settings get global device_name | tr -d '\r')
adb shell am start -W -a android.settings.DEVICE_NAME > /dev/null
maestro "$FLOWS/device-name-field.yaml"
maestro -e TEXT="$COPIED" "$FLOWS/field-shows.yaml"
BEFORE=$(clip_offers)
adb shell input keycombination 113 29 # Ctrl+A
adb shell input keycombination 113 31 # Ctrl+C
wait_for 20 "the copy in Settings never reached the desktop" eval '(( $(clip_offers) > BEFORE ))'
CLIP_ID=$(grep -A2 'member=ClipboardOffered' "$OUT/signals.txt" | grep -o 'uint64 [0-9]*' | tail -1 | cut -d' ' -f2)
PULLED=$(python3 "$ROOT/tests/e2e/link_clipboard_dbus.py" pull "$ID" "$CLIP_ID" "text/plain;charset=utf-8") \
  || fail "PullClipboard"
[[ $PULLED == "$COPIED" ]] || fail "the desktop got \"$PULLED\", not \"$COPIED\""
screenshot 8-copied-in-settings
record "{\"step\":\"android-copy-reaches-desktop\",\"text\":\"$COPIED\",\"taps\":0}"

OFFERS=$(clip_offers)
python3 "$ROOT/tests/e2e/link_clipboard_dbus.py" offer "text/plain;charset=utf-8" "from the desktop clipboard" \
  || fail "OfferClipboard"
sleep 3 # real time: the phone sets it, and any echo would come back by now
[[ $(clip_offers) == "$OFFERS" ]] || fail "the phone echoed the desktop clip back"
adb shell input keycombination 113 29 # Ctrl+A
adb shell input keycombination 113 50 # Ctrl+V
maestro -e TEXT="from the desktop clipboard" "$FLOWS/field-shows.yaml"
screenshot 9-pasted-in-settings
adb shell input keyevent KEYCODE_BACK
adb shell input keyevent KEYCODE_BACK
adb shell input keyevent KEYCODE_HOME
record '{"step":"desktop-copy-reaches-android","pasted":"from the desktop clipboard","echo":false}'

# The one-tap path without the grants: "Send to desktop" in the text-selection menu.
BEFORE=$(clip_offers)
adb shell am start -W -n "$PACKAGE/.clipboard.ClipboardReadActivity" -a android.intent.action.PROCESS_TEXT \
  -t text/plain --es android.intent.extra.PROCESS_TEXT "'selected on the phone'" > /dev/null
wait_for 20 "the selected text never reached the desktop" eval '(( $(clip_offers) > BEFORE ))'
CLIP_ID=$(grep -A2 'member=ClipboardOffered' "$OUT/signals.txt" | grep -o 'uint64 [0-9]*' | tail -1 | cut -d' ' -f2)
PULLED=$(python3 "$ROOT/tests/e2e/link_clipboard_dbus.py" pull "$ID" "$CLIP_ID" "text/plain;charset=utf-8") \
  || fail "PullClipboard for the selection"
[[ $PULLED == "selected on the phone" ]] || fail "the selection arrived as \"$PULLED\""
record '{"step":"process-text-reaches-desktop","text":"selected on the phone"}'

maestro -e STAY=off "$FLOWS/stay-connected.yaml"
wait_for 10 "the Stay connected notification outlived the toggle" gone "Connected to your desktops"
record '{"step":"stay-connected-in-background","service_notification":"only while on"}'

maestro "$FLOWS/unpair.yaml"
wait_for 15 "desktop still lists the emulator after it unpaired: $(devices)" eval '! devices | grep -q "[0-9a-f]\{32\}"'
record '{"step":"unpaired-from-android"}'

python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
steps = [json.loads(line) for line in open(os.path.join(out, "results.jsonl"))]
shots = sorted(name for name in os.listdir(out) if name.endswith(".png"))
json.dump({"pass": True, "steps": steps, "screenshots": shots}, open(os.path.join(out, "results.json"), "w"), indent=2)
PY
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
