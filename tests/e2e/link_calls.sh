#!/usr/bin/env bash
# Phone calls on the desktop, through the shell, against a real umbriel-linkd, a present headless phone, and a desktop
# MPRIS test player. Proves: a ringing call shows an incoming-call notification with the contact's name, or the
# number when there is no name; Mute ringer and Decline reach the phone as call-action; the desktop's playing player is
# paused while the call rings or is active and resumed when it is idle; a declined call ends and its notification goes;
# D-Bus refuses an unknown action. Every message is validated against protocol/link-v1/messages.cddl. Writes call-*.png,
# steps.txt, hold.jsonl, player-calls.txt, transcript.jsonl, linkd.log, and noctalia.log to $OUT (default
# ./artifacts/link-calls).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-calls}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
boot_headless 1

with_noctalia '
  BIN='"$LINK_BIN"'
  E2E='"$ROOT"'/tests/e2e
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  say() { printf "%s\n" "$*" >&3; }
  phone_saw() { grep -q "$1" "$OUT/hold.jsonl"; }
  player_calls() { [[ $(grep -cx "$1" "$OUT/player-calls.txt" 2> /dev/null) -ge $2 ]]; }
  link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}" 2>&1; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  python3 "$E2E/mpris_test_player.py" "$OUT/player-calls.txt" &
  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
    hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  wait_for "the test player never reached the phone" phone_saw "\"player\":\"e2e\""

  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0100\",\"name\":\"Ada Lovelace\"}"
  wait_for "the playing desktop player was not paused for the call" player_calls Pause 1
  sleep 1.5 # real time: the toast maps and paints
  grim "$OUT/call-ringing-contact.png"
  [[ $(msg notification-action-latest call:mute) == ok ]] || fail "Mute ringer: $(msg notification-action-latest call:mute)"
  wait_for "the phone did not get mute" phone_saw "\"action\":\"mute\",\"desktop\":\"[0-9a-f]*\",\"event\":\"call-action\""
  step "ringing call from Ada Lovelace: notification, desktop player paused, Mute ringer reached the phone"
  say "call {\"state\":\"active\"}"
  sleep 0.5
  say "call {\"state\":\"idle\"}"
  wait_for "the desktop player was not resumed after the call" player_calls Play 1
  step "call answered then ended: desktop player resumed"

  say "call {\"state\":\"ringing\",\"number\":\"+1 555 0199\"}"
  wait_for "the desktop player was not paused for the second call" player_calls Pause 2
  sleep 1.5 # real time: the toast maps and paints
  grim "$OUT/call-ringing-number.png"
  [[ $(msg notification-action-latest call:decline) == ok ]] || fail "Decline"
  wait_for "the phone did not get decline" phone_saw "\"action\":\"decline\""
  wait_for "the declined call did not end the pause" player_calls Play 2
  sleep 1 # real time: the notification closes
  grim "$OUT/call-declined.png"
  [[ $(msg notification-action-latest call:mute) == *"no active notification"* ]] || fail "the declined call left its notification"
  step "ringing call from a number without a contact: Decline ended it, notification gone, player resumed"

  OUTPUT=$(link CallAction "$ID" answer) && fail "an unknown call action was sent"
  [[ $OUTPUT == *Error.Rejected* ]] || fail "unknown call action: $OUTPUT"
  step "dbus: an unknown call action is rejected"
  kill "$hold"
  wait "$hold" || true
'
cat "$OUT/steps.txt"
BIN=${LINK_BIN:-$ROOT/link/target/debug}
"$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl" \
  | tee "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
for shot in call-ringing-contact call-ringing-number call-declined; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
