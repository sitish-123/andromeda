#!/usr/bin/env bash
# File transfers through the shell against a real umbriel-linkd and a present headless phone: a phone's offer becomes a
# notification whose Accept action accepts it over D-Bus, the transfer shows a progress notification, and the result
# a notification whose Open and Show in folder actions open the received file and its folder through the default
# handlers (a recording stand-in in the private HOME); a declined offer reaches the phone as declined; link-send-file
# opens a file in the shell, passes its descriptor, and the phone receives it intact. Writes notification-offer.png,
# notification-progress.png, notification-done.png, steps.txt, hold.jsonl, linkd.log, and phone.log to $OUT
# (default ./artifacts/link-files-shell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-files-shell}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl"

mkdir -p "$RUNTIME/home/.config" "$RUNTIME/home/.local/share/applications"
cat > "$RUNTIME/record-open" <<SH
#!/bin/sh
printf '%s\n' "\$1" >> "$RUNTIME/opened.txt"
SH
chmod +x "$RUNTIME/record-open"
printf '[Desktop Entry]\nType=Application\nName=E2E opener\nExec=%s %%u\nMimeType=text/plain;inode/directory;\n' \
  "$RUNTIME/record-open" > "$RUNTIME/home/.local/share/applications/e2e-opener.desktop"
printf '[Default Applications]\ntext/plain=e2e-opener.desktop\ninode/directory=e2e-opener.desktop\n' \
  > "$RUNTIME/home/.config/mimeapps.list"

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 300); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  say() { printf "%s\n" "$*" >&3; }
  DOWNLOADS=$RUNTIME/downloads
  PHONE_DL=$RUNTIME/phone-downloads
  mkdir -p "$DOWNLOADS" "$PHONE_DL" "$RUNTIME/src"

  XDG_STATE_HOME=$RUNTIME/linkd XDG_DOWNLOAD_DIR=$DOWNLOADS "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
    >> "$OUT/phone.log" 2>&1 || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --downloads "$PHONE_DL" hold --on-offer accept \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"

  head -c 536870912 /dev/urandom > "$RUNTIME/src/notes.txt"
  say "send $RUNTIME/src/notes.txt"
  sleep 2 # real time: the offer arrives after hashing, then the toast maps and paints
  grim "$OUT/notification-offer.png"
  wait_for "invoking Accept" eval "[[ \$(msg notification-invoke-latest accept) == ok ]]"
  wait_for "the accepted file never started" eval "ls -A \"$DOWNLOADS\" | grep -q linkpart"
  sleep 1.5 # real time: the progress toast maps and paints
  grim "$OUT/notification-progress.png"
  step "accepted from the notification; progress shown"
  wait_for "the file never arrived" test -f "$DOWNLOADS/notes.txt"
  [[ $(sha256sum < "$RUNTIME/src/notes.txt") == $(sha256sum < "$DOWNLOADS/notes.txt") ]] || fail "the received file differs"
  sleep 1.5 # real time: the result toast maps and paints
  grim "$OUT/notification-done.png"
  [[ $(msg notification-invoke-latest folder) == ok ]] || fail "invoking Show in folder"
  wait_for "Show in folder opened nothing" eval "[[ -s \$RUNTIME/opened.txt ]]"
  [[ $(tail -1 "$RUNTIME/opened.txt" | sed "s,^file://,,") == "$DOWNLOADS" ]] || fail "Show in folder opened $(cat "$RUNTIME/opened.txt")"
  step "done notification; Show in folder opened $(tail -1 "$RUNTIME/opened.txt")"
  rm -f "$DOWNLOADS/notes.txt"

  printf "second" > "$RUNTIME/src/second.txt"
  say "send $RUNTIME/src/second.txt"
  wait_for "no offer notification for the second file" eval "[[ \$(msg notification-invoke-latest decline) == ok ]]"
  wait_for "the phone never saw the decline" grep -q "\"status\":\"declined\"" "$OUT/hold.jsonl"
  step "declined from the notification"

  printf "third" > "$RUNTIME/src/third.txt"
  say "send $RUNTIME/src/third.txt"
  sleep 2 # real time: hashing, then the offer toast
  wait_for "invoking Accept for the third file" eval "[[ \$(msg notification-invoke-latest accept) == ok ]]"
  wait_for "the third file never arrived" test -f "$DOWNLOADS/third.txt"
  sleep 1 # real time: the result toast
  wait_for "invoking Open" eval "[[ \$(msg notification-invoke-latest open) == ok ]]"
  wait_for "Open did not open the file" eval "[[ \$(tail -1 \$RUNTIME/opened.txt | sed s,^file://,,) == \"$DOWNLOADS/third.txt\" ]]"
  step "Open opened $(tail -1 "$RUNTIME/opened.txt")"

  printf "from the desktop" > "$RUNTIME/src/desk.txt"
  [[ $(msg link-send-file "$ID" "$RUNTIME/src/desk.txt") == ok ]] || fail "link-send-file: $(msg link-send-file "$ID" "$RUNTIME/src/desk.txt")"
  wait_for "the phone never received desk.txt" test -f "$PHONE_DL/desk.txt"
  [[ $(cat "$PHONE_DL/desk.txt") == "from the desktop" ]] || fail "the phone received $(cat "$PHONE_DL/desk.txt")"
  [[ $(msg link-send-file "$ID" "$RUNTIME/src/missing.txt") == *"cannot open"* ]] || fail "link-send-file of a missing file"
  step "link-send-file delivered desk.txt to the phone"
'
cat "$OUT/steps.txt"
for shot in notification-offer notification-progress notification-done; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
