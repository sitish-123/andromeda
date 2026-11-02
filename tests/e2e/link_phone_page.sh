#!/usr/bin/env bash
# Control Center phone page (Link in the shell), against a real andromeda-linkd and a present headless phone: the page
# opens from the quick sheet, shows the phone, and its Ring row makes the phone ring; a phone that goes away shows
# Not connected. Shots go to $OUT (default ./artifacts/link-phone-page).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-phone-page}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  phone_saw() { [[ $(grep -c "$1" "$OUT/hold.jsonl") -ge ${2:-1} ]]; }
  ringing() { [[ $(msg link-ringing) == "$1" ]]; }
  click() { "$POINTER" 1280 720 move "$1" "$2" > /dev/null; "$POINTER" 1280 720 move "$1" "$2" click 272 move 5 715 > /dev/null; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  msg theme-mode-set "${THEME:-light}" > /dev/null
  msg panel-open control-center quick > /dev/null
  sleep 1 # real time: the sheet opens
  grim "$OUT/phone-disconnected-card.png"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold --status 82,0,wifi \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  sleep 1 # real time: the card repaints
  grim "$OUT/phone-card.png"

  msg panel-open control-center detail-phone > /dev/null
  sleep 1 # real time: the page opens
  grim "$OUT/phone-connected.png"
  click "${RING_X:-1000}" "${RING_Y:-180}"
  wait_for "pressing Ring did not ring the phone" phone_saw "\"event\":\"ring\",\"on\":true"
  wait_for "the shell does not show the phone ringing" ringing "phone $ID"
  click "${RING_X:-1000}" "${RING_Y:-180}"
  wait_for "pressing Stop did not stop the phone" phone_saw "\"event\":\"ring\",\"on\":false"

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still shown connected" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" || \$(msg link-devices) != *\" connected \"* ]]"
  sleep 1 # real time: the page repaints
  grim "$OUT/phone-disconnected.png"
'
echo "PASS; artifacts: $OUT"
