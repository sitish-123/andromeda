#!/usr/bin/env bash
# Greeter mode parity with the old greeter (GM6; old cases face_greeter.sh G8-G10, password only) on two outputs:
# both outputs show the greeter; keys reach the password field after clicks anywhere; Enter on an empty field posts
# nothing; a session picked from the session chip is the one that starts. SHOT=1 only writes start.png, SHOT=2 only
# menu.png with the session menu open (to find the coordinates), then stop. Writes start.png, picked.png, requests.jsonl and greeter.log to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/greeter-mode-parity}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 2
fail() { echo "FAIL: $*" >&2; exit 1; }
wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; run grim "$OUT/fail.png" || true; fail "$what"; }
count() { grep -c "\"type\": \"$1\"" "$OUT/requests.jsonl" 2> /dev/null || true; }
atleast() { [[ $(count "$1") -ge $2 ]]; }
posted() { grep -q "\"response\": \"$1\"" "$OUT/requests.jsonl" 2> /dev/null; }
P=$ROOT/compositor/build-debug/tests/pointer-client
# pointer-client's 1280x720 absolute extent spans the whole 2560x720 layout: x is halved.
click() { run "$P" 1280 720 move "$(($1 / 2))" "$2" click 272 > /dev/null 2>&1; sleep 0.5; }
# Mean brightness of the clock area on one output (x offset $1): the greeter draws its clock on every output.
lit() { run grim -g "$(($1 + 560)),190 160x60" -t ppm - | tail -c 28800 | od -An -tu1 -v | awk '{for(i=1;i<=NF;i++){s+=$i;n++}}END{exit !(s/n > 8)}'; }

run wtype -s 3600000 > /dev/null 2>&1 &
sock=$RUNTIME/greetd.sock
MOCK_SCENARIOS=plain,plain,plain python3 -I "$ROOT/tests/e2e/mock_greetd.py" "$sock" "$OUT/requests.jsonl" > "$OUT/mock.log" 2>&1 &
wait_for "the mock did not start" grep -q ready "$OUT/mock.log"
mkdir -p "$RUNTIME/share/wayland-sessions" "$RUNTIME/greeter-state"
cp "$ROOT/compositor/data/andromeda.desktop" "$RUNTIME/share/wayland-sessions/"
printf '[Desktop Entry]\nName=Other Desktop\nExec=other-session\nType=Application\n' \
  > "$RUNTIME/share/wayland-sessions/other.desktop"
run env XDG_DATA_DIRS="$RUNTIME/share" NOCTALIA_GREETER_STATE_DIR="$RUNTIME/greeter-state" GREETD_SOCK="$sock" \
  "$NOCTALIA" greeter --user "$USER" > "$OUT/greeter.log" 2>&1 &
greeter=$!
wait_for "no session opened at start" atleast create_session 1
wait_for "no first frame" grep -q 'first frame at' "$OUT/greeter.log"
sleep 1
run grim "$OUT/start.png"
[[ ${SHOT:-0} == 1 ]] && { kill "$greeter"; echo "shot only"; exit 0; }
CHIP_X=${CHIP_X:-1126}
CHIP_Y=${CHIP_Y:-673}
if [[ ${SHOT:-0} == 2 ]]; then
  click "$CHIP_X" "$CHIP_Y"
  run grim "$OUT/menu.png"
  kill "$greeter"; echo "menu shot only"; exit 0
fi

lit 0 || fail "the first output shows no clock"
lit 1280 || fail "the second output shows no clock"


# G8: clicks on empty spots keep the keyboard on the password field.
click 200 600
click 1100 650
run wtype "wrong" -k Return
wait_for "typing after clicks did not reach the password" posted wrong
wait_for "no new session after the wrong password" atleast create_session 2

# G9: Enter on an empty field posts nothing.
run wtype -k Return
sleep 0.5
grep -q '"response": ""' "$OUT/requests.jsonl" && fail "Enter on an empty field posted an empty password"

# G10: the picked session starts.
click "$CHIP_X" "$CHIP_Y"
click "${ITEM_X:-1011}" "${ITEM_Y:-625}"
run grim "$OUT/picked.png"
run wtype "right" -k Return
wait_for "no start_session" atleast start_session 1
grep '"type": "start_session"' "$OUT/requests.jsonl" | grep -q 'other-session' \
  || fail "the picked session did not start: $(grep start_session "$OUT/requests.jsonl")"
wait_for "the greeter did not exit after the session start" bash -c "! kill -0 $greeter 2> /dev/null"
echo "pass greeter_mode_parity"
