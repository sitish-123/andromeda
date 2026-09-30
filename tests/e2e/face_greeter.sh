#!/usr/bin/env bash
# Greeter face login (docs/face.md G1-G5) against mock_greetd.py. The greeter opens the PAM session as soon as the
# user is shown: a face match starts the session with no password posted; a password submitted during the face check
# is posted the moment the secret prompt opens; a wrong password clears the field and starts a new session at once;
# with nothing enrolled it waits for the password; a face that gives up shows the reason and posts nothing. Writes
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

# start CASE SCENARIOS: a mock greetd playing SCENARIOS and a greeter for the current user.
start() {
  case=$1
  local sock=$RUNTIME/greetd-$case.sock
  MOCK_SCENARIOS=$2 python3 "$MOCK" "$sock" "$OUT/requests-$case.jsonl" > "$OUT/mock-$case.log" 2>&1 &
  mock=$!
  wait_for "$case: the mock did not start" grep -q ready "$OUT/mock-$case.log"
  run env NOCTALIA_GREETER_LOG=stderr GREETD_SOCK="$sock" "$GREETER" --user "$USER" > "$OUT/greeter-$case.log" 2>&1 &
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

echo "PASS: greeter face login G1-G5 against a mock greetd; artifacts: $OUT"
