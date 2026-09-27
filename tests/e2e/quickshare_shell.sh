#!/usr/bin/env bash
# Quick Share through the shell against a real umbriel-linkd on the test bus: quickshare-visible turns it on; the
# control center's Devices tab and Settings' Phone & Devices page show the switch; a file sent by our Quick Share sender
# raises an offer notification with the sender and PIN, and nothing is saved until its Accept button is pressed; then
# the file is in XDG_DOWNLOAD_DIR and a "Files from" notification offers Open and Show in folder. Sending the other
# way, quickshare-nearby finds a receiver and quickshare-send delivers a file to it with a "Sent to" notification. While visible the
# daemon advertises on the host's LAN for the few seconds the test runs. Writes devices.png, settings.png, offer.png,
# received.png, sent.png, steps.txt, linkd.log, send.log to $OUT (default ./artifacts/quickshare-shell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/quickshare-shell}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/send.log"
mkdir -p "$RUNTIME/home/.config" "$RUNTIME/home/Incoming"
echo 'XDG_DOWNLOAD_DIR="$HOME/Incoming"' > "$RUNTIME/home/.config/user-dirs.dirs"
head -c 3000000 /dev/urandom > "$RUNTIME/photo.jpg"

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 100); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  ocr() { magick "$1" -resize 200% png:- | tesseract - - 2> /dev/null; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw Quick Share" eval "[[ \$(msg quickshare-visible) == off ]]"
  [[ $(msg quickshare-visible on) == on ]] || fail "quickshare-visible on refused"
  wait_for "visibility did not reach the shell" eval "[[ \$(msg quickshare-visible) == on ]]"
  step "visible: $(msg quickshare-visible)"

  msg panel-open control-center devices > /dev/null
  sleep 1.5 # real time: the control center maps and paints
  grim "$OUT/devices.png"
  ocr "$OUT/devices.png" | grep -q "Quick Share" || fail "no Quick Share card in the Devices tab"
  msg panel-close > /dev/null

  msg settings-open devices > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/settings.png"
  SETTINGS_TEXT=$(ocr "$OUT/settings.png")
  echo "$SETTINGS_TEXT" | grep -q "Visible to nearby devices" || fail "no Quick Share switch in Settings"
  ! echo "$SETTINGS_TEXT" | grep -q "No settings found" || fail "the page also shows the empty state"
  msg settings-close > /dev/null
  step "switch shown in the Devices tab and Settings"

  "$BIN/umbriel-quickshare" --name "Test Phone" send --addr 127.0.0.1:4718 "$RUNTIME/photo.jpg" \
    > "$RUNTIME/send.jsonl" 2>> "$OUT/send.log" &
  SENDER=$!
  wait_for "no connection reached the daemon" grep -q "quick share: connection" "$OUT/linkd.log"
  sleep 1 # real time: the toast maps and paints
  grim "$OUT/offer.png"
  OFFER_TEXT=$(ocr "$OUT/offer.png")
  echo "$OFFER_TEXT" | grep -q "Test Phone wants to share" || fail "no offer notification: $OFFER_TEXT"
  PIN=$(sed -n "s/.*\"pin\":\"\([0-9]*\)\".*/\1/p" "$RUNTIME/send.jsonl")
  echo "$OFFER_TEXT" | grep -q "PIN $PIN" || fail "the notification does not show the sender PIN $PIN"
  [[ -z $(ls -A "$RUNTIME/home/Incoming") ]] || fail "bytes saved before Accept"
  step "offer shown with PIN $PIN"

  [[ $(msg notification-invoke-latest accept) == ok ]] || fail "could not press Accept"
  wait "$SENDER" || fail "the sender failed after Accept"
  cmp -s "$RUNTIME/photo.jpg" "$RUNTIME/home/Incoming/photo.jpg" || fail "saved file differs"
  sleep 1 # real time: the result toast maps and paints
  grim "$OUT/received.png"
  ocr "$OUT/received.png" | grep -q "Files from Test Phone" || fail "no received notification"
  step "accepted and saved; result notification shown"

  mkdir -p "$RUNTIME/nearby-in"
  "$BIN/umbriel-quickshare" --name "Nearby Phone" receive --dir "$RUNTIME/nearby-in" --port 47310 --consent accept \
    > "$RUNTIME/nearby.jsonl" 2>> "$OUT/send.log" &
  [[ $(msg quickshare-nearby on) == ok ]] || fail "quickshare-nearby on refused"
  wait_for "the shell never listed the nearby receiver" eval "msg quickshare-nearby | grep -q \"Nearby Phone\""
  PEER=$(msg quickshare-nearby | grep "Nearby Phone" | cut -d" " -f1)
  [[ $(msg quickshare-send "$PEER" "$RUNTIME/photo.jpg") == ok ]] || fail "quickshare-send refused"
  wait_for "the nearby phone did not get the file" test -f "$RUNTIME/nearby-in/photo.jpg"
  cmp -s "$RUNTIME/photo.jpg" "$RUNTIME/nearby-in/photo.jpg" || fail "the sent file differs"
  sleep 1 # real time: the sent toast maps and paints
  grim "$OUT/sent.png"
  ocr "$OUT/sent.png" | grep -q "Sent to Nearby Phone" || fail "no sent notification"
  msg quickshare-nearby off > /dev/null
  step "sent to a nearby receiver found by discovery; sent notification shown"
'
cat "$OUT/steps.txt"
echo "PASS; artifacts: $OUT"
