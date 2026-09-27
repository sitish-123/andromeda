#!/usr/bin/env bash
# Slice C on the Android emulator against a private umbriel-linkd (own dbus-daemon, own XDG_STATE_HOME). Proves:
# with notification access granted, a notification posted with `cmd notification post` reaches D-Bus
# NotificationPosted with its app name and icon, and a D-Bus dismissal cancels it on the phone and comes back as
# NotificationRemoved; a chat notification (the fixture app) is mirrored with its RemoteInput reply action, a reply sent
# with D-Bus NotificationAction reaches the RemoteInput, and the updated conversation comes back to D-Bus. A media
# session on the phone (the fixture app) becomes the MPRIS player umbriel_link_<device> with its title and artwork,
# and playerctl's pause, next, and seek reach that session; a desktop MPRIS test player shows on the app's Media
# screen, and its Pause button reaches it as PlayPause. D-Bus Ring rings the phone on the alarm stream at full volume
# while Do Not Disturb (priority, which lets alarms through) stays as the user set it, and a stop puts the volume back; the app's Ring desktop reaches D-Bus as
# RingRequested, and once the desktop reports ringing, Stop ringing stops it.
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
FIXTURE_APK=${FIXTURE_APK:-$ROOT/link/android/fixture/build/outputs/apk/debug/fixture-debug.apk}
FLOWS=$ROOT/link/android/maestro
PACKAGE=org.umbriel.link

rm -rf "$OUT"
mkdir -p "$OUT"
# A fresh directory per run: dbus-daemon unlinks its socket path on exit, so a reused path would race the old one.
RUNTIME=$(mktemp -d /tmp/link-android-features.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; "$ADB" -s "$SERIAL" logcat -d > "$OUT/logcat.txt" 2>&1 || true; exit 1; }
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
[[ -s $FIXTURE_APK ]] || fail "no fixture APK at $FIXTURE_APK"

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

# A fresh install: replacing the package cancels its notifications late, racing the reply below.
adb uninstall org.umbriel.link.fixture > /dev/null 2>&1 || true
adb install "$FIXTURE_APK" > /dev/null || fail "installing $FIXTURE_APK"
adb shell pm grant org.umbriel.link.fixture android.permission.POST_NOTIFICATIONS
adb logcat -c
adb shell am start -W -n org.umbriel.link.fixture/.MessageActivity > /dev/null
# The key names the fresh install's uid, so a notification a previous run left behind cannot stand in for it.
FIXTURE_KEY="0|org.umbriel.link.fixture|7|null|$(adb shell pm list packages -U org.umbriel.link.fixture | sed 's/.*uid://' | tr -d '\r')"
wait_for 30 "no NotificationPosted for the chat message" heard NotificationPosted "$FIXTURE_KEY"
CHAT_KEY=$(posted_field "$FIXTURE_KEY" key)
REPLY=$(posted_field "$FIXTURE_KEY" reply)
[[ $(signal NotificationPosted "$FIXTURE_KEY") == *"Are we still on for dinner?"* ]] || fail "the chat text did not arrive"
[[ -n $REPLY ]] || fail "the chat notification carries no RemoteInput action: $(signal NotificationPosted "Are we still on for dinner?")"
link NotificationAction "$ID" "$CHAT_KEY" "$REPLY" "'Yes, leaving now'" > /dev/null || fail "D-Bus NotificationAction reply"
wait_for 20 "the RemoteInput did not receive the reply" eval 'adb logcat -d -s LinkFixture | grep -q "reply Yes, leaving now"'
wait_for 20 "the replied conversation did not come back to the desktop" heard NotificationPosted "Yes, leaving now"
adb shell cmd statusbar expand-notifications
sleep 2 # real time: the shade animates open
screenshot 1-reply-on-phone
adb shell cmd statusbar collapse
record "{\"step\":\"reply-through-remote-input\",\"action\":\"$REPLY\",\"text\":\"Yes, leaving now\"}"

adb shell am start -W -n org.umbriel.link.fixture/.PlayerActivity > /dev/null
PLAYER=umbriel_link_$ID
pc() { playerctl -p "$PLAYER" "$@" 2> /dev/null || true; }
fixture() { adb logcat -d -s LinkFixture | grep -q "$1"; }
wait_for 30 "the phone's media session never reached MPRIS: $(playerctl -l 2>&1)" eval '[[ $(pc metadata title) == "Emulator Song" ]]'
[[ $(pc metadata artist) == "The Emulators" && $(pc status) == Playing ]] || fail "MPRIS: $(pc metadata artist) $(pc status)"
ART=$(pc metadata mpris:artUrl)
[[ $ART == file://*.jpg && -s ${ART#file://} ]] || fail "no artwork file: $ART"
pc pause
wait_for 10 "the media session was not paused" fixture " pause"
wait_for 10 "MPRIS does not show the phone paused: $(pc status)" eval '[[ $(pc status) == Paused ]]'
pc next
wait_for 10 "the media session did not skip" fixture " next"
wait_for 10 "the next title did not reach MPRIS" eval '[[ $(pc metadata title) == "Emulator Song, part 2" ]]'
pc position 90
wait_for 10 "the media session was not seeked" fixture " seek 90000"
record "{\"step\":\"phone-media-on-mpris\",\"player\":\"$PLAYER\",\"commands\":[\"pause\",\"next\",\"seek\"],\"art\":\"jpeg\"}"

python3 "$ROOT/tests/e2e/mpris_test_player.py" "$OUT/player-calls.txt" &
adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
maestro "$FLOWS/media.yaml"
wait_for 10 "the app's Pause did not reach the desktop player" grep -qx PlayPause "$OUT/player-calls.txt"
record '{"step":"desktop-media-on-phone","player":"E2E Player","command":"PlayPause"}'


alarm_volume() { adb shell cmd media_session volume --stream 4 --get 2> /dev/null | sed -n 's/.*volume is \([0-9]*\).*/\1/p' | tr -d '\r'; }
zen() { adb shell settings get global zen_mode | tr -d '\r'; }
adb shell cmd notification allow_dnd "$PACKAGE"
# The emulator ignores `cmd media_session volume` for the alarm stream, which stays at its maximum, so the restore
# is checked against whatever it was.
VOLUME_BEFORE=$(alarm_volume)
adb shell cmd notification set_dnd priority
wait_for 10 "Do Not Disturb did not turn on" eval '[[ $(zen) != 0 ]]'
ZEN_BEFORE=$(zen)
link Ring "$ID" true > /dev/null || fail "D-Bus Ring"
wait_for 10 "the phone did not report ringing" heard PhoneRinging "'$ID', true"
wait_for 10 "the alarm stream is not at full volume: $(alarm_volume)" eval '[[ $(alarm_volume) == 7 ]]'
[[ $(zen) == "$ZEN_BEFORE" ]] || fail "the ring changed the user's Do Not Disturb on Android 15: zen_mode $(zen)"
adb shell dumpsys audio > "$OUT/audio.txt"
PIID=$(grep "new AudioAttributes:AudioAttributes: usage=USAGE_ALARM" "$OUT/audio.txt" | tail -1 | sed -n 's/.*player piid:\([0-9]*\).*/\1/p')
[[ -n $PIID ]] && grep -q "player piid:$PIID event:started" "$OUT/audio.txt" || fail "no alarm player started in dumpsys audio"
adb shell cmd statusbar expand-notifications
sleep 2 # real time: the shade animates open
screenshot 4-phone-ringing
adb shell cmd statusbar collapse
link Ring "$ID" false > /dev/null || fail "D-Bus Ring off"
wait_for 10 "the phone did not report the stop" heard PhoneRinging "'$ID', false"
wait_for 10 "the alarm player was not released" eval 'adb shell dumpsys audio | grep -q "releasing player piid:$PIID"'
[[ $(alarm_volume) == "$VOLUME_BEFORE" ]] || fail "the alarm volume was not restored: $(alarm_volume), was $VOLUME_BEFORE"
[[ $(zen) == "$ZEN_BEFORE" ]] || fail "Do Not Disturb was not restored: zen_mode $(zen)"
adb shell cmd notification set_dnd off
record "{\"step\":\"find-my-phone\",\"alarm_volume\":\"$VOLUME_BEFORE -> 7 -> $(alarm_volume)\",\"dnd\":\"priority, untouched\",\"player\":\"USAGE_ALARM piid $PIID, released on stop\"}"

adb shell am start -W -n "$PACKAGE/.MainActivity" > /dev/null
maestro "$FLOWS/ring-desktop.yaml"
wait_for 10 "the phone did not ring the desktop" heard RingRequested "'$ID', true"
link DesktopRinging "$ID" true > /dev/null || fail "D-Bus DesktopRinging"
maestro "$FLOWS/stop-desktop.yaml"
wait_for 10 "the phone did not stop the desktop" heard RingRequested "'$ID', false"
record '{"step":"find-my-desktop","ring":"RingRequested true","stop":"RingRequested false"}'

python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
steps = [json.loads(line) for line in open(os.path.join(out, "results.jsonl"))]
shots = sorted(name for name in os.listdir(out) if name.endswith(".png"))
json.dump({"pass": True, "steps": steps, "screenshots": shots}, open(os.path.join(out, "results.json"), "w"), indent=2)
PY
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
