#!/usr/bin/env bash
# Shares through the shell against a real umbriel-linkd and a present headless phone: text from the phone becomes a
# notification whose default action puts it on the clipboard (read back with wl-paste); a link becomes a notification
# whose default action opens it through the default https handler (a recording stand-in in the private HOME);
# link-share sends text and a link to the phone, and refuses a disconnected phone. Writes notification-text.png,
# notification-link.png, steps.txt, hold.jsonl, linkd.log, and phone.log to $OUT (default ./artifacts/link-share-shell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-share-shell}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl"

mkdir -p "$RUNTIME/home/.config" "$RUNTIME/home/.local/share/applications"
cat > "$RUNTIME/record-url" <<SH
#!/bin/sh
printf '%s\n' "\$1" >> "$RUNTIME/opened.txt"
SH
chmod +x "$RUNTIME/record-url"
printf '[Desktop Entry]\nType=Application\nName=E2E browser\nExec=%s %%u\nMimeType=x-scheme-handler/https;\n' \
  "$RUNTIME/record-url" > "$RUNTIME/home/.local/share/applications/e2e-browser.desktop"
printf '[Default Applications]\nx-scheme-handler/https=e2e-browser.desktop\n' > "$RUNTIME/home/.config/mimeapps.list"

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
  phone() { "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  say() { printf "%s\n" "$*" >&3; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  [[ $(msg link-share "$ID" text hello) == *"is not connected"* ]] || fail "link-share to a disconnected phone: $(msg link-share "$ID" text hello)"
  step "disconnected: $(msg link-share "$ID" text hello)"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold < "$RUNTIME/phone.in" \
    > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  step "present: $(msg link-devices)"

  say "text Meet at the station at 6"
  sleep 1.5 # real time: the toast maps and paints
  grim "$OUT/notification-text.png"
  [[ $(msg notification-invoke-latest) == ok ]] || fail "invoking the text notification"
  wait_for "clipboard does not hold the shared text" eval "[[ \$(wl-paste -n 2> /dev/null) == \"Meet at the station at 6\" ]]"
  step "text notification copied: $(wl-paste -n)"

  say "link https://example.org/e2e-share"
  sleep 1.5 # real time: the toast maps and paints
  grim "$OUT/notification-link.png"
  [[ $(msg notification-invoke-latest) == ok ]] || fail "invoking the link notification"
  wait_for "the link was not opened" eval "[[ -s \$RUNTIME/opened.txt ]]"
  [[ $(cat "$RUNTIME/opened.txt") == "https://example.org/e2e-share" ]] || fail "opened $(cat "$RUNTIME/opened.txt")"
  step "link notification opened: $(cat "$RUNTIME/opened.txt")"

  [[ $(msg link-share "$ID" text sent from the shell) == ok ]] || fail "link-share text"
  [[ $(msg link-share "$ID" link https://example.org/from-shell) == ok ]] || fail "link-share link"
  wait_for "phone missed the shell text" grep -q "\"text\":\"sent from the shell\"" "$OUT/hold.jsonl"
  wait_for "phone missed the shell link" grep -q "\"text\":\"https://example.org/from-shell\"" "$OUT/hold.jsonl"
  [[ $(msg link-share "$ID" file x) == *"kind is text or link"* ]] || fail "link-share accepted kind file"
  step "shell to phone: $(grep -c "\"event\":\"received\"" "$OUT/hold.jsonl") shares received"

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still connected after it stopped" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"
  step "stopped: $(msg link-devices)"
'
cat "$OUT/steps.txt"
for shot in notification-text notification-link; do [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }; done
echo "PASS; artifacts: $OUT"
