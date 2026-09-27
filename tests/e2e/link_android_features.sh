#!/usr/bin/env bash
# Slice C on the Android emulator against a private umbriel-linkd (own dbus-daemon, own XDG_STATE_HOME). Proves:
# with notification access granted, a notification posted with `cmd notification post` reaches D-Bus
# NotificationPosted with its app name and icon, and a D-Bus dismissal cancels it on the phone and comes back as
# NotificationRemoved; an SMS arriving in Google Messages is mirrored with its RemoteInput reply action, and a reply
# sent with D-Bus NotificationAction is delivered through that RemoteInput (Messages records it as sent).
# Writes screenshots, results.jsonl, results.json, signals.txt, notifications.txt, linkd.log to $OUT
# (default ./artifacts/link-android-features). Needs the debug APK and a running emulator; run under
# flock /tmp/link-emulator.lock, since the emulator is shared.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-android-features}
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
RUNTIME=$(mktemp -d /tmp/link-android-features.XXXX)
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
# One line per signal: gdbus monitor prints each with all its arguments.
signal() { grep -F "org.umbriel.Link1.$1 " "$OUT/signals.txt" | grep -F -- "$2" | tail -1; }
heard() { [[ -n $(signal "$1" "$2") ]]; }
posted() { notifications | grep -qF "$1"; }
gone() { ! notifications | grep -qF "$1"; }
# Messages shows a sent reply in its notification, or at least stores it with the conversation.
replied() { posted "$1" || adb shell content query --uri content://sms --projection body 2> /dev/null | grep -qF "$1"; }
# The notification key and the first reply action of the latest NotificationPosted matching $1.
posted_field() {
  signal NotificationPosted "$1" | python3 -c '
import re, sys
line = sys.stdin.read()
args = re.findall(r"'"'"'((?:[^'"'"'\\]|\\.)*)'"'"'", line)
replies = re.findall(r"\('"'"'([^'"'"']*)'"'"', '"'"'[^'"'"']*'"'"', true\)", line)
print({"key": args[1], "app": args[2], "reply": replies[0] if replies else ""}[sys.argv[1]])' "$2"
}

XDG_STATE_HOME=$RUNTIME/desktop "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
gdbus monitor --session -d org.umbriel.Link1 -o /org/umbriel/Link1 > "$OUT/signals.txt" 2>&1 &
wait_for 5 "gdbus monitor did not start" test -s "$OUT/signals.txt"

adb shell cmd statusbar collapse
adb shell input keyevent KEYCODE_HOME
adb uninstall "$PACKAGE" > /dev/null 2>&1 || true
adb install "$APK" > /dev/null || fail "installing $APK"
adb shell cmd notification allow_listener "$PACKAGE/$PACKAGE.notifications.MirrorService"
record '{"step":"installed","notification_access":true}'

URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
adb shell am start -W -a android.intent.action.VIEW -d "'$URI'" "$PACKAGE" > /dev/null
maestro "$FLOWS/pair.yaml"
wait_for 20 "D-Bus does not show the emulator connected: $(devices)" connected
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
record "{\"step\":\"paired-and-connected\",\"device\":\"$ID\"}"

adb shell cmd notification post -S bigtext -t "'Build finished'" e2e-build "'All 42 checks passed'" > /dev/null
wait_for 20 "no NotificationPosted for the adb notification" heard NotificationPosted "All 42 checks passed"
KEY=$(posted_field "All 42 checks passed" key)
APP=$(posted_field "All 42 checks passed" app)
signal NotificationPosted "All 42 checks passed" | grep -q "\[byte 0x89, 0x50, 0x4e, 0x47" || fail "no PNG icon on the mirrored notification"
record "{\"step\":\"cmd-notification-mirrored\",\"key\":\"$KEY\",\"app\":\"$APP\",\"icon\":\"png\"}"

link NotificationDismiss "$ID" "$KEY" > /dev/null || fail "D-Bus NotificationDismiss"
wait_for 10 "the dismissal did not cancel the phone notification" gone "All 42 checks passed"
wait_for 10 "no NotificationRemoved after the dismissal" heard NotificationRemoved "$KEY"
record '{"step":"dismissed-from-desktop","phone":"cancelled","dbus":"NotificationRemoved"}'

adb emu sms send 5551234 "'Are you still coming tonight?'" > /dev/null
wait_for 60 "no NotificationPosted for the SMS" heard NotificationPosted "Are you still coming tonight?"
SMS_KEY=$(posted_field "Are you still coming tonight?" key)
REPLY=$(posted_field "Are you still coming tonight?" reply)
[[ -n $REPLY ]] || fail "the SMS notification carries no RemoteInput action: $(signal NotificationPosted "Are you still coming tonight?")"
adb shell cmd statusbar expand-notifications
sleep 2 # real time: the shade animates open
screenshot 1-sms-on-phone
adb shell cmd statusbar collapse
link NotificationAction "$ID" "$SMS_KEY" "$REPLY" "'Yes, leaving now'" > /dev/null || fail "D-Bus NotificationAction reply"
wait_for 30 "Messages did not take the reply" replied "Yes, leaving now"
adb shell cmd statusbar expand-notifications
sleep 2 # real time: the shade animates open
screenshot 2-sms-replied
adb shell cmd statusbar collapse
record "{\"step\":\"reply-through-remote-input\",\"app\":\"$(posted_field "Are you still coming tonight?" app)\",\"action\":\"$REPLY\"}"

python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
steps = [json.loads(line) for line in open(os.path.join(out, "results.jsonl"))]
shots = sorted(name for name in os.listdir(out) if name.endswith(".png"))
json.dump({"pass": True, "steps": steps, "screenshots": shots}, open(os.path.join(out, "results.json"), "w"), indent=2)
PY
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
