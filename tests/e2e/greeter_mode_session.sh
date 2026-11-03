#!/usr/bin/env bash
# greetd's view of greeter mode: session/greetd/andromeda-greeter-session starts a headless `andromeda --greeter`
# whose only client is `noctalia greeter`. Greeter mode hides virtual keyboards, so nothing can type: the mock's face
# scenario authenticates without a password. The greeter starts the session and exits, and the compositor (the
# whole greeter session) ends with it. Writes requests.jsonl and session.log to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/greeter-mode-session}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-debug/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-debug/noctalia}
RUNTIME=$(mktemp -d /tmp/dsk.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
rm -rf "$OUT"
mkdir -p "$OUT"
fail() { echo "FAIL: $*" >&2; exit 1; }
wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }

sock=$RUNTIME/greetd.sock
MOCK_SCENARIOS=face python3 -I "$ROOT/tests/e2e/mock_greetd.py" "$sock" "$OUT/requests.jsonl" > "$OUT/mock.log" 2>&1 &
wait_for "the mock did not start" grep -q ready "$OUT/mock.log"
mkdir -p "$RUNTIME/share/wayland-sessions" "$RUNTIME/state"
cp "$ROOT/compositor/data/andromeda.desktop" "$RUNTIME/share/wayland-sessions/"

# The script owns XDG_RUNTIME_DIR (/tmp/noctalia-runtime-UID); the greeter finds the mock through GREETD_SOCK.
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1 \
  NOCTALIA_BIN="$NOCTALIA" NOCTALIA_GREETER_COMPOSITOR_BIN="$ANDROMEDA" NOCTALIA_GREETER_LOG=stderr \
  GREETD_SOCK="$sock" XDG_DATA_DIRS="$RUNTIME/share" NOCTALIA_GREETER_STATE_DIR="$RUNTIME/state" \
  "$ROOT/session/greetd/andromeda-greeter-session" --user "$USER" > "$OUT/session.log" 2>&1 &
session=$!
wait_for "no create_session from the greeter" grep -q '"type": "create_session"' "$OUT/requests.jsonl"
wait_for "the greeter drew no frame" grep -q 'first frame at' "$OUT/session.log"
grep -q 'greeter: started pid=' "$OUT/session.log" || fail "andromeda did not run the greeter as its child"

wait_for "no start_session" grep -q '"type": "start_session"' "$OUT/requests.jsonl"
wait_for "the greeter session did not end after the session start" bash -c "! kill -0 $session 2> /dev/null"
wait "$session" || fail "the greeter session exited with an error"
echo "pass greeter_mode_session"
