#!/usr/bin/env bash
# Greeter face login (docs/face.md G1-G7) against mock_greetd.py, with mock_gaze.py on a private system bus. The greeter opens the PAM session as soon as the
# user is shown: a face match starts the session with no password posted; a password submitted during the face check
# is posted the moment the secret prompt opens; a wrong password clears the field and starts a new session at once;
# with nothing enrolled it waits for the password; a face that gives up shows the reason and posts nothing. With the
# patched Gaze the greeter registers gdm-face as a marker host; gazed's match answers the open prompt with
# GAZE_CONFIRMED, and a password typed at once is posted at once. Under Ryoku (G8-G12): keys reach the password field
# after taps anywhere, Enter on an empty field sends nothing, the session list picks the session that starts (Umbriel
# when nothing was chosen), a match waits while a list is open, and a face that gives up says so. Writes
# requests-*.jsonl and greeter-*.log to $OUT (default ./artifacts/face-greeter).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/face-greeter}
source "$(dirname "$0")/lib.sh"
GREETER=${GREETER:-$ROOT/greeter/build/noctalia-greeter}
MOCK="$ROOT/tests/e2e/mock_greetd.py"
rm -rf "$OUT"
boot_headless 1
fail() { echo "FAIL: $*" >&2; exit 1; }
wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }
requests() { cat "$OUT/requests-$case.jsonl" 2> /dev/null || true; }
count() { requests | grep -c "\"type\": \"$1\"" || true; }
posted() { requests | grep -c "\"response\": \"$1\"" || true; }
atleast() { [[ $(count "$1") -ge $2 ]]; }
posted_atleast() { [[ $(posted "$1") -ge $2 ]]; }
run wtype -s 3600000 > /dev/null 2>&1 &

# A private system bus, so the greeter registers with the mock and never with the machine's gazed.
dbus-daemon --config-file="$RUNTIME/bus.conf" --nofork --print-address=3 3> "$RUNTIME/system-bus" &
wait_for "no private system bus" test -s "$RUNTIME/system-bus"
export DBUS_SYSTEM_BUS_ADDRESS=$(head -1 "$RUNTIME/system-bus")
python3 "$ROOT/tests/e2e/mock_gaze.py" "$OUT/gaze-calls.txt" > "$OUT/mock-gaze.log" 2>&1 &
wait_for "the gazed mock did not start" grep -q ready "$OUT/mock-gaze.log"
P=$ROOT/compositor/build-debug/tests/pointer-client
click() { run "$P" 1280 720 move "$1" "$2" click 272 > /dev/null 2>&1; sleep 0.5; }
started_with() { requests | grep '"type": "start_session"' | grep -qi "$1"; }
gaze_miss() {
  busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze Finish sssb \
    verify-no-match no-face no-face false > /dev/null
}
gaze_match() {
  busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze Finish sssb \
    verify-match usable usable true > /dev/null
}

# start CASE SCENARIOS: a mock greetd playing SCENARIOS and a greeter for the current user.
start() {
  case=$1
  local sock=$RUNTIME/greetd-$case.sock
  MOCK_SCENARIOS=$2 python3 "$MOCK" "$sock" "$OUT/requests-$case.jsonl" > "$OUT/mock-$case.log" 2>&1 &
  mock=$!
  wait_for "$case: the mock did not start" grep -q ready "$OUT/mock-$case.log"
  mkdir -p "$RUNTIME/greeter-state"
  run env NOCTALIA_GREETER_STATE_DIR="$RUNTIME/greeter-state" DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" NOCTALIA_GREETER_LOG=stderr GREETD_SOCK="$sock" "$GREETER" --user "$USER" > "$OUT/greeter-$case.log" 2>&1 &
  greeter=$!
  wait_for "$case: no session opened on start" atleast create_session 1
  sleep 1.6 # real time: the entry animation drops typed keys until it ends
}
stop() { kill "$greeter" "$mock" 2> /dev/null || true; wait "$greeter" "$mock" 2> /dev/null || true; }

start G1 face
wait_for "G1: a face match did not start the session" atleast start_session 1
[[ $(requests | grep -c '"response": "') == 0 ]] || fail "G1: a password was posted"
stop

start G2 noface
run wtype right
run wtype -k Return
wait_for "G2: the queued password was not posted" posted_atleast right 1
wait_for "G2: no session after the queued password" atleast start_session 1
stop

start G3 plain,face
run wtype wrong
run wtype -k Return
wait_for "G3: the wrong password was not posted" posted_atleast wrong 1
wait_for "G3: the failed session was not cancelled" atleast cancel_session 1
wait_for "G3: no new session after the failure" atleast create_session 2
wait_for "G3: the retried face match did not start the session" atleast start_session 1
stop

start G4 plain
sleep 1
[[ $(requests | grep -c '"response": "') == 0 ]] || fail "G4: answered the prompt before any typing"
run wtype right
run wtype -k Return
wait_for "G4: the password was not posted" posted_atleast right 1
wait_for "G4: no session after the password" atleast start_session 1
stop

start G5 noface
wait_for "G5: the give-up reason was not shown" grep -q "Face not detected" "$OUT/greeter-G5.log"
sleep 0.5
[[ $(posted right) == 0 && $(count start_session) == 0 ]] || fail "G5: logged in without the user"
stop

start G6 race
wait_for "G6: the greeter did not register as marker host" grep -q "^AddPamInternal gdm-face" "$OUT/gaze-calls.txt"
wait_for "G6: the prompt was not left open" grep -q "open prompt\|PAM secret message" "$OUT/greeter-G6.log"
gaze_match
wait_for "G6: the match did not answer the prompt" posted_atleast GAZE_CONFIRMED 1
wait_for "G6: no session after the match" atleast start_session 1
stop

start G7 race
run wtype right
run wtype -k Return
wait_for "G7: the password was not posted at once" posted_atleast right 1
wait_for "G7: no session after the password" atleast start_session 1
stop

# Coordinates are for the 1280x720 headless output: the session name at the top right, its list below it, the hint
# line at the bottom right, and an empty spot in the middle.
start G8 race
click 700 400
click 1150 560
run wtype right
run wtype -k Return
wait_for "G8: typing after taps did not reach the password" posted_atleast right 1
wait_for "G8: no session after the password" atleast start_session 1
started_with umbriel || fail "G8: the default session was not Umbriel ($(requests | grep start_session))"
stop

start G9 race
run wtype -k Return
sleep 0.5
[[ $(requests | grep -c '"response": ""') == 0 ]] || fail "G9: Enter on an empty field sent an empty password"
run wtype right
run wtype -k Return
wait_for "G9: the password after an empty Enter was not posted" posted_atleast right 1
stop

start G10 race
click 1000 63
click 1000 111
run wtype right
run wtype -k Return
wait_for "G10: no session after picking one" atleast start_session 1
started_with umbriel && fail "G10: the picked session did not start ($(requests | grep start_session))"
stop

start G11 race
click 1000 63
gaze_match
sleep 1
[[ $(posted GAZE_CONFIRMED) == 0 ]] || fail "G11: a match logged in while the session list was open"
click 1000 63
wait_for "G11: the match did not log in once the list closed" posted_atleast GAZE_CONFIRMED 1
stop

start G12 race
gaze_miss
wait_for "G12: the give-up was not shown" grep -q "face check ended without a match (no-face)" "$OUT/greeter-G12.log"
stop

echo "PASS: greeter face login G1-G12 against a mock greetd; artifacts: $OUT"
