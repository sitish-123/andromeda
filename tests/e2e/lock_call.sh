#!/usr/bin/env bash
# Rulebook 9.15 call card on the lock screen, against a real andromeda-linkd, a headless phone and mock_gaze.py. Proves:
# a call that rings while locked shows the call card; Decline on the card does not reach the phone while the session
# is locked; the next unlock (face match) declines that call; an unlock without Decline leaves a ringing call alone;
# the same card and Decline work with the lock delegated to `noctalia lock`. No password is ever submitted. Writes
# call-card.png, decline-pending.png, call-card-process.png, media-process.png, steps.txt, hold.jsonl, calls.txt to $OUT
# (default ./artifacts/lock-call).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-call}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
KEYS=${KEYS:-$ROOT/compositor/build-debug/tests/pointer-client}
rm -rf "$OUT"
boot_headless 1

with_noctalia '
  BIN='"$LINK_BIN"'
  KEYS='"$KEYS"'
  E2E='"$ROOT"'/tests/e2e
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  say() { printf "%s\n" "$*" >&3; }
  declines() { grep -c "\"action\":\"decline\"" "$OUT/hold.jsonl" 2> /dev/null || true; }
  activity_has() { [[ $(msg live-activity) == *"$1"* ]]; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze "$@" > /dev/null; }
  called() { grep -c "^$1" "$OUT/calls.txt" 2> /dev/null || true; }
  verifying() { [[ $(called VerifyStartFor) -ge 1 ]]; }
  locked() { msg status | grep -q "\"locked\": true"; }
  unlocked() { msg status | grep -q "\"locked\": false"; }
  lock() { : > "$OUT/calls.txt"; [[ $(msg session lock) == ok ]] || fail "lock refused"; wait_for "did not lock" locked; }
  face_unlock() {
    wtype -k Return # face on request: Enter with an empty field starts one scan
    wait_for "$1: a key press did not arm face unlock" verifying
    gaze /com/gundulabs/Gaze dsk.test.Gaze Finish sssb verify-match usable usable true
    wait_for "$1: verify-match did not unlock" unlocked
  }

  # A keyboard must exist before the lock surface maps, or the surface never receives keyboard focus.
  wtype -s 3600000 &
  sleep 0.3
  python3 "$E2E/mock_gaze.py" "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the gaze mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"

  lock
  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0100\",\"name\":\"Ada Lovelace\"}"
  wait_for "the ringing call never reached the shell" activity_has "Ada Lovelace"
  sleep 2 # real time: the enter transition ends (the surface takes no input before) and the card paints
  grim "$OUT/call-card.png"
  # Decline: the left capsule of the card at 1280 x 720, scale 1 (card bottom 32 above the avatar).
  "$KEYS" 1280 720 move 546 340 click 272 > /dev/null
  sleep 1 # real time: a decline sent now would reach the phone well within this
  [[ $(declines) == 0 ]] || fail "Decline reached the phone while locked"
  grim "$OUT/decline-pending.png"
  step "ringing while locked: call card shown; Decline did not reach the phone while locked"
  face_unlock decline
  wait_for "the unlock did not decline the call" eval "[[ \$(declines) == 1 ]]"
  step "face unlock after Decline: the call was declined"

  say "call {\"state\":\"idle\"}"
  lock
  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0199\"}"
  wait_for "the second call never reached the shell" activity_has "555 0199"
  sleep 2 # real time: the enter transition ends (the surface takes no input before)
  face_unlock plain
  sleep 1 # real time: a stray decline would reach the phone well within this
  [[ $(declines) == 1 ]] || fail "an unlock without Decline declined the call"
  step "unlock without Decline: the ringing call stays"

  # Delegated: the lock process (`noctalia lock`) draws the lock; its own read-only Link1 client feeds the card.
  say "call {\"state\":\"idle\"}"
  "$NOCTALIA" lock > "$OUT/lock-process.log" 2>&1 &
  lockproc=$!
  wait_for "the shell does not delegate to the lock process" grep -q "delegates every lock" "$OUT/noctalia.log"
  lock
  grep -q "lock requested (user)" "$OUT/lock-process.log" || fail "the lock process did not take the lock"
  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0142\",\"name\":\"Grace Hopper\"}"
  wait_for "the third call never reached the shell" activity_has "Grace Hopper"
  sleep 2 # real time: the enter transition ends (the surface takes no input before) and the card paints
  grim "$OUT/call-card-process.png"
  "$KEYS" 1280 720 move 546 340 click 272 > /dev/null
  sleep 1 # real time: a decline sent now would reach the phone well within this
  [[ $(declines) == 1 ]] || fail "Decline reached the phone while the lock process held the lock"
  face_unlock process
  wait_for "the lock process did not decline the call on unlock" eval "[[ \$(declines) == 2 ]]"
  step "lock process: call card shown; Decline reached the daemon on unlock"
  say "call {\"state\":\"idle\"}"
  python3 "$E2E/mpris_test_player.py" "$OUT/player-calls.txt" > /dev/null 2>&1 &
  player=$!
  ! grep -q "session bus:" "$OUT/lock-process.log" || fail "the lock process has no session services"
  lock
  sleep 2 # real time: the enter transition ends and the media card paints
  grim "$OUT/media-process.png"
  face_unlock media
  step "lock process: media card shot with the shell running"
  kill "$player"
  kill "$lockproc"
  wait "$lockproc" || true
  kill "$hold"
  wait "$hold" || true
'
cat "$OUT/steps.txt"
for shot in call-card decline-pending call-card-process media-process; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
