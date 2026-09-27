#!/usr/bin/env bash
# The Android app on an emulator against a private umbriel-linkd (own dbus-daemon, own XDG_STATE_HOME), reached through
# the emulator's NAT at the host's LAN addresses in the pairing URI. Proves: QR pairing from the opened URI (Maestro
# taps Pair and grants notifications), the app shows Connected and D-Bus Devices agrees; text and a link shared from
# Android through the share target arrive as D-Bus Received; text and a link sent with D-Bus Share post Android
# notifications, and tapping the text's (its Copy PendingIntent) clears it; leaving the app ends presence (D-Bus shows it
# disconnected) and returning restores it; "Stay connected" keeps it connected in the background with its
# foreground-service notification, which goes away when turned off; unpairing in the app removes it on the desktop.
# Writes screenshots, results.jsonl, results.json, signals.txt, notifications.txt, linkd.log to $OUT
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

XDG_STATE_HOME=$RUNTIME/desktop "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
dbus-monitor --session "type='signal',interface='org.umbriel.Link1',member='Received'" > "$OUT/signals.txt" 2>&1 &
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

adb shell input keyevent KEYCODE_HOME
wait_for 15 "presence outlived the app leaving the foreground" disconnected
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
wait_for 20 "presence did not return with the app" connected
record '{"step":"presence-follows-foreground"}'

maestro -e STAY=on "$FLOWS/stay-connected.yaml"
wait_for 10 "no Stay connected notification" posted "Connected to your desktops"
adb shell input keyevent KEYCODE_HOME
sleep 5 # real time: longer than the foreground check above waited for a disconnect
connected || fail "Stay connected did not keep the session in the background: $(devices)"
screenshot 6-stay-connected-background
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
