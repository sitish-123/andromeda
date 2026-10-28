#!/usr/bin/env bash
# Notification sync between a headless phone and a real andromeda-linkd and andromeda-link-session, shown through a
# stand-in notification server. Proves: changes made while the phone is away arrive on reconnect through the snapshot
# (a removal closes, an update replaces in place and alerts, an alert-once update replaces without a sound, a new one
# shows); an unchanged reconnect shows nothing; after a daemon restart nothing shows again, and a removal made while
# the daemon was down still closes. Then: a phone reporting notification access off gets a notice, closed when access
# is back; a locked desktop shows a count, and at unlock the held posts show quietly; with "show while locked" on they
# show at once; a sixth on show folds the oldest into "1 more from <phone>". The notifier plays the lock screen.
# Every message is validated against link/api/messages.cddl.
# Writes steps.txt, notifier.jsonl, hold.jsonl, transcript.jsonl, signals.txt, linkd.log, and session.log to $OUT
# (default ./artifacts/link-notifications-sync).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-notifications-sync}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
E2E=$ROOT/tests/e2e

if [[ ${1:-} != inner ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  RUNTIME=$(mktemp -d /tmp/link-notifications-sync.XXXX)
  trap 'rm -rf "$RUNTIME"' EXIT
  # No service directories: a call while the test daemon is down must not start the installed one.
  cat > "$RUNTIME/bus.conf" << 'CONF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
  env -u DBUS_SESSION_BUS_ADDRESS OUT="$OUT" RUNTIME="$RUNTIME" BIN="$BIN" E2E="$E2E" ROOT="$ROOT" \
    XDG_RUNTIME_DIR="$RUNTIME" XDG_STATE_HOME="$RUNTIME/state" HOME="$RUNTIME/home" \
    dbus-run-session --config-file="$RUNTIME/bus.conf" -- "$0" inner
  cat "$OUT/steps.txt"
  "$BIN/andromeda-link-phone" check-transcript "$ROOT/link/api/messages.cddl" "$OUT/transcript.jsonl" \
    > "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
  echo "PASS; artifacts: $OUT"
  exit 0
fi

trap 'kill $(jobs -p) 2> /dev/null || true' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
wait_for() {
  local what=$1; shift
  for _ in $(seq 150); do "$@" && return; sleep 0.1; done
  fail "$what"
}
link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
devices() {
  gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get \
    org.andromeda.Link1 Devices 2> /dev/null
}
connected() { [[ $(devices) == *"'E2E Phone', true"* ]]; }
say() { printf "%s\n" "$*" >&3; }
post() { say "notify {\"id\":\"$1\",\"app\":\"Messages\",\"title\":\"$2\",\"text\":\"$3\"${4:+,$4}}"; }
# The notifier's calls as "notify <shown id> <body> <quiet|alert>", "link <shown id> <summary> <quiet|alert>" for
# Link's own notices, and "close <shown id>", in order.
calls() {
  python3 -c '
import json, sys
for line in open(sys.argv[1]):
    call = json.loads(line)
    sound = "quiet" if "suppress-sound" in call.get("hints", []) else "alert"
    if call["event"] == "notify" and call["app"] == "Link":
        print("link", call["id"], call["summary"], sound)
    elif call["event"] == "notify":
        print("notify", call["id"], call["body"], sound)
    elif call["event"] == "close":
        print("close", call["id"])' "$OUT/notifier.jsonl"
}
count() { calls | grep -c "^$1" || true; }
shown_id() { calls | grep "^notify .* $1 " | head -1 | cut -d" " -f2; }
linkd() { "$BIN/andromeda-linkd" >> "$OUT/linkd.log" 2>&1 & LINKD=$!; }

mkfifo "$RUNTIME/notifier.in"
exec 4<> "$RUNTIME/notifier.in"
python3 "$E2E/link_notifier.py" "$OUT/notifier.jsonl" < "$RUNTIME/notifier.in" 2> "$OUT/notifier.log" &
wait_for "the notifier did not start" grep -qs ready "$OUT/notifier.jsonl"
linkd
wait_for "andromeda-linkd did not start" eval "devices > /dev/null"
dbus-monitor --session "type=signal,interface=org.andromeda.Link1" > "$OUT/signals.txt" 2>&1 &
PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['port'])" "$RUNTIME/state/andromeda-link/devices.json")
CODE=$(link StartPairing | cut -d"'" -f2)
"$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
  >> "$OUT/phone.log" 2>&1 || fail "pairing"
"$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
  hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
wait_for "the phone did not connect" connected

post one Ann "Coming at 6?"
post two Bob "Build failed"
post three Upload "10%"
wait_for "the three posts did not show" eval '[[ $(count notify) == 3 ]]'
ONE=$(shown_id "Coming") TWO=$(shown_id "Build") THREE=$(shown_id "10%")
step "posted 3: shown as $ONE, $TWO, $THREE, each alerting"

say "present off"
wait_for "the phone did not disconnect" eval '! connected'
say "unnotify one"
post two Bob "Build fixed"
post three Upload "60%" '"alert_once":true'
post four Cat "Dinner?"
sleep 1 # real time: nothing may reach the desktop while away
[[ $(count notify) == 3 && $(count close) == 0 ]] || fail "a change reached the desktop while away: $(calls)"
say "present on"
wait_for "the snapshot did not close one" eval 'calls | grep -qx "close $ONE"'
wait_for "the update of two did not replace it" eval 'calls | grep -qx "notify $TWO Build fixed alert"'
wait_for "the alert-once update of three did not replace it quietly" eval 'calls | grep -qx "notify $THREE 60% quiet"'
wait_for "four did not show" eval 'calls | grep -q "^notify [0-9]* Dinner? alert$"'
sleep 1 # real time: a duplicate would follow at once
[[ $(count notify) == 6 && $(count close) == 1 ]] || fail "the reconnect showed more than the changes: $(calls)"
step "away: removed one, updated two, alert-once three, posted four; reconnect closed $ONE, replaced $TWO (alert) and $THREE (quiet), showed four"

say "present off"
wait_for "the phone did not disconnect" eval '! connected'
say "present on"
wait_for "the phone did not reconnect" connected
sleep 1 # real time: a re-show would follow the connect at once
[[ $(count notify) == 6 && $(count close) == 1 ]] || fail "an unchanged reconnect showed something: $(calls)"
step "unchanged reconnect: nothing shown"

FOUR=$(shown_id "Dinner?")
kill "$LINKD"
wait "$LINKD" || true
say "unnotify four"
linkd
wait_for "andromeda-linkd did not restart" eval "devices > /dev/null"
say "present off"
say "present on"
wait_for "the phone did not reconnect after the restart" connected
wait_for "four, removed while the daemon was down, did not close" eval 'calls | grep -qx "close $FOUR"'
sleep 1 # real time: a re-show would follow the connect at once
[[ $(count notify) == 6 && $(count close) == 2 ]] || fail "the restart showed something again: $(calls)"
[[ -s $RUNTIME/state/andromeda-link/notifications/$(ls "$RUNTIME/state/andromeda-link/notifications") ]] \
  || fail "no notification store on disk"
step "daemon restart: nothing shown again; four, removed while it was down, closed ($FOUR)"

link_line() { calls | grep "^link [0-9]* $1 $2$" | tail -1 | cut -d" " -f2; }
say "status 80,0,wifi,,notifications-off"
wait_for "no notice that notification access is off" eval '[[ -n $(link_line "Notification access is off on E2E Phone" alert) ]]'
NOTICE=$(link_line "Notification access is off on E2E Phone" alert)
say "status 80,0,wifi"
wait_for "the access notice did not close when access came back" eval 'calls | grep -qx "close $NOTICE"'
step "access off: notice $NOTICE shown, closed when access came back"

printf "lock on\n" >&4
post locked-1 Eve "Lunch?"
post locked-2 Eve "At 1?"
wait_for "no count while locked" eval '[[ -n $(link_line "2 phone notifications" quiet) ]]'
COUNT=$(link_line "2 phone notifications" quiet)
calls | grep -q "^notify .* Lunch? " && fail "a post showed in full while locked: $(calls)"
printf "lock off\n" >&4
wait_for "the count did not close at unlock" eval 'calls | grep -qx "close $COUNT"'
wait_for "the held posts did not show quietly at unlock" \
  eval 'calls | grep -q "^notify [0-9]* Lunch? quiet$" && calls | grep -q "^notify [0-9]* At 1? quiet$"'
step "locked: a count ($COUNT) instead of the posts; at unlock the count closed and both showed quietly"

link SetNotificationsWhileLocked true > /dev/null
printf "lock on\n" >&4
post locked-3 Eve "Running late"
wait_for "with the setting on, a post did not show while locked" eval 'calls | grep -q "^notify [0-9]* Running late alert$"'
printf "lock off\n" >&4
link SetNotificationsWhileLocked false > /dev/null
step "setting on: a post showed in full while locked"

# Five on show (three, two, locked-1..3, in the order last shown); the sixth folds the oldest, three, into the summary.
post capped Fay "One too many"
wait_for "the sixth did not fold the oldest into a summary" eval '[[ -n $(link_line "1 more from E2E Phone" quiet) ]]'
SUMMARY=$(link_line "1 more from E2E Phone" quiet)
wait_for "the oldest did not close" eval 'calls | grep -qx "close $THREE"'
say "unnotify three"
wait_for "the summary did not close when its last folded one went" eval 'calls | grep -qx "close $SUMMARY"'
step "cap: the sixth on show folded the oldest into \"1 more from E2E Phone\" ($SUMMARY), closed once it went"
