#!/usr/bin/env bash
# Shell greeter mode (`noctalia greeter`, .agent/DESIGN.md "Shell greeter mode") against mock_greetd.py: the greeter
# opens a session for the user at start; a wrong password is posted, rejected, and a new session opens; the right
# password starts the session from andromeda.desktop and the greeter exits 0; the cursor stays visible (G0). Writes requests.jsonl and greeter.log
# to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/greeter-mode}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
fail() { echo "FAIL: $*" >&2; exit 1; }
wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }
count() { grep -c "\"type\": \"$1\"" "$OUT/requests.jsonl" 2> /dev/null || true; }
atleast() { [[ $(count "$1") -ge $2 ]]; }
posted() { grep -q "\"response\": \"$1\"" "$OUT/requests.jsonl" 2> /dev/null; }

# A long-lived virtual keyboard: the first key of a new wtype keyboard is lost while its keymap loads.
run wtype -s 3600000 > /dev/null 2>&1 &
sock=$RUNTIME/greetd.sock
MOCK_SCENARIOS=plain,plain python3 -I "$ROOT/tests/e2e/mock_greetd.py" "$sock" "$OUT/requests.jsonl" > "$OUT/mock.log" 2>&1 &
wait_for "the mock did not start" grep -q ready "$OUT/mock.log"
mkdir -p "$RUNTIME/share/wayland-sessions" "$RUNTIME/greeter-state"
cp "$ROOT/compositor/data/andromeda.desktop" "$RUNTIME/share/wayland-sessions/"
run env XDG_DATA_DIRS="$RUNTIME/share" NOCTALIA_GREETER_STATE_DIR="$RUNTIME/greeter-state" GREETD_SOCK="$sock" \
  "$NOCTALIA" greeter --user "$USER" > "$OUT/greeter.log" 2>&1 &
greeter=$!
wait_for "no session opened at start" atleast create_session 1
wait_for "no first frame" grep -q 'first frame at' "$OUT/greeter.log"

# G0: the cursor stays visible over the greeter (the client sets its shape on pointer enter).
run "$ROOT/compositor/build-debug/tests/pointer-client" 1280 720 move 300 300 > /dev/null 2>&1 || true
cursor_drawn() { run "$ANDROMEDA" cursor-state --json | jq -e '(.ok // .).image != null' > /dev/null; }
wait_for "no cursor image over the greeter" cursor_drawn

run wtype "wrong" -k Return
wait_for "the wrong password was not posted" posted wrong
wait_for "no new session after the wrong password" atleast create_session 2
atleast start_session 1 && fail "a wrong password started the session"

run wtype "right" -k Return
wait_for "the right password was not posted" posted right
wait_for "no start_session" atleast start_session 1
grep '"type": "start_session"' "$OUT/requests.jsonl" | grep -q '"cmd": \["start-andromeda"\]' || fail "start_session did not run start-andromeda"
wait_for "the greeter did not exit after the session start" bash -c "! kill -0 $greeter 2> /dev/null"
wait "$greeter" || fail "the greeter exited with an error"
grep -q 'right' "$OUT/greeter.log" && fail "the password reached the log"
echo "pass greeter_mode"
