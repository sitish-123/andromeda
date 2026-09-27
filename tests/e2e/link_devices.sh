#!/usr/bin/env bash
# The control center's Devices tab against a real umbriel-linkd started after the shell: link-devices refuses while
# the daemon is absent and answers once it owns its name; link-pair opens the pairing view (QR code, 6-digit code,
# countdown); the QR code read off the screenshot (zbarimg) is the daemon's URI, and the headless phone pairs with it;
# link-devices lists the phone, then shows it connected while it holds a session; link-unpair removes it; stopping the
# daemon makes the shell report it gone. Writes pairing.png, devices.png, steps.txt, linkd.log, and phone.log to $OUT
# (default ./artifacts/link-devices).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-devices}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log"

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  # wait_for DESCRIPTION CMD...: poll a shell IPC answer, which follows D-Bus signals asynchronously.
  wait_for() {
    local what=$1; shift
    for _ in $(seq 100); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }

  [[ $(msg link-devices) == *"not running"* ]] || fail "link-devices answered without a daemon: $(msg link-devices)"
  step "no daemon: $(msg link-devices)"

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  linkd=$!
  wait_for "shell never saw umbriel-linkd appear" eval "[[ \$(msg link-devices) != *error* ]]"
  step "daemon up: devices=[$(msg link-devices)]"

  [[ $(msg link-pair) == ok ]] || fail "link-pair: $(msg link-pair)"
  wait_for "no pairing window in the shell" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE URI <<< "$(msg link-pairing)"
  [[ $CODE =~ ^[0-9]{6}$ && $URI == umbriel-link:pair\?* ]] || fail "bad pairing state: $(msg link-pairing)"
  step "pairing: code=$CODE uri=$URI"
  sleep 1.5 # real time: the control center maps and paints the QR code
  grim "$OUT/pairing.png"
  SCANNED=$(zbarimg -q --raw "$OUT/pairing.png") || fail "no QR code on screen"
  [[ $SCANNED == "$URI" ]] || fail "on-screen QR code reads $SCANNED"
  step "scanned from pairing.png: $SCANNED"

  if ! phone pair --uri "$SCANNED" >> "$OUT/phone.log"; then
    # No LAN address in the URI was reachable; a fresh window over loopback proves the same shell path.
    step "uri unreachable, pairing by code over loopback"
    PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
    [[ $(msg link-pair) == ok ]] || fail "second link-pair"
    wait_for "no second window" eval "[[ \$(msg link-pairing) == open\ * && \$(msg link-pairing) != *\$CODE* ]]"
    read -r _ CODE URI <<< "$(msg link-pairing)"
    phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "code pairing failed"
  fi
  wait_for "shell did not see PairingFinished" eval "[[ \$(msg link-pairing) == \"paired E2E Phone\" ]]"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) =~ ^[0-9a-f]{32}\ disconnected\ E2E\ Phone$ ]]"
  step "paired: $(msg link-pairing); devices=[$(msg link-devices)]"
  ID=$(msg link-devices | cut -d" " -f1)

  phone connect --hold 3 >> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  step "session: devices=[$(msg link-devices)]"
  sleep 1 # real time: the tab repaints from PropertiesChanged
  grim "$OUT/devices.png"
  wait "$hold"
  wait_for "phone still connected after its session ended" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"

  [[ $(msg link-unpair "$ID") == ok ]] || fail "link-unpair: $(msg link-unpair "$ID")"
  wait_for "phone still listed after unpair" eval "[[ -z \$(msg link-devices) ]]"
  step "unpaired: devices=[$(msg link-devices)]"
  [[ $(msg link-unpair "$ID") == *"no paired device"* ]] || fail "unpairing twice was accepted"

  kill "$linkd"
  wait "$linkd" || true
  wait_for "shell kept the daemon after it left the bus" eval "[[ \$(msg link-devices) == *\"not running\"* ]]"
  step "daemon gone: $(msg link-devices)"
'
cat "$OUT/steps.txt"
for shot in pairing devices; do [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }; done
echo "PASS; artifacts: $OUT"
