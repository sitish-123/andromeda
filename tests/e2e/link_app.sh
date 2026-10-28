#!/usr/bin/env bash
# The Link app against a private andromeda-linkd and the headless phone, driven from the keyboard:
# before the service runs, Home says Link is not running; with the service and no phone, No phone yet; Pair a phone
# opens a dialog with the code and a QR code, and pairing the phone with that code closes it. With the phone away the
# header says Not connected and the composer is off. Once the phone holds with a status and a player, the header shows
# Wi-Fi, 82%, charging, 3 bars, and the player; the seam shows a notification with Reply, a missed call, and the
# phone's file offer. Text typed in the composer and Send clipboard both reach the phone.
# Writes app-down.png, app-empty.png, app-pairing.png, app-away.png, app-player.png, app-light.png, app-dark.png, steps.txt,
# hold.jsonl, linkd.log, session.log, app.log, and phone.log to $OUT (default ./artifacts/link-app).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-app}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl" "$OUT/app.log"

with_noctalia '
  BIN='"$LINK_BIN"'
  DATA='"$ROOT/link/crates/link-app/data"'
  fail() { grim "$OUT/app-fail.png" || true; echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 200); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  say() { printf "%s\n" "$*" >&3; }
  shot() { sleep 1.5; grim "$OUT/app-$1.png"; } # real time: the window repaints
  # -s: wtype waits after its keymap upload, else the compositor drops the first key.
  key() { wtype -s 300 "$@"; }
  app() { GTK_A11Y=none ANDROMEDA_LINK_DATA=$DATA ADW_DEBUG_COLOR_SCHEME=$1 "$BIN/andromeda-link" >> "$OUT/app.log" 2>&1 & app_pid=$!; }
  clips() { grep -c "\"event\":\"clip-offered\"" "$OUT/hold.jsonl" || true; }

  app prefer-light
  shot down
  step "down: Home before andromeda-linkd runs"

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  "$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
  wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  shot empty
  step "empty: the service runs and no phone is paired"

  prop() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 "$1"; }
  key -k Return
  wait_for "Pair a phone opened no pairing window" eval "[[ \$(prop Pairing) == *true* ]]"
  shot pairing
  # The phone scans the QR code on the screen, as the camera would.
  URI=$(zbarimg -q --raw "$OUT/app-pairing.png") || fail "no QR code on the screen"
  step "pairing: the dialog shows a QR code for ${URI%%&*}"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" pair --uri "$URI" --addr "127.0.0.1:$PORT" \
    >> "$OUT/phone.log" 2>&1 || fail "pairing with the QR code the app showed"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"OnePlus 12\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  wait_for "the pairing session never ended" eval "[[ \$(msg link-devices) == \"$ID disconnected OnePlus 12\" ]]"
  shot away
  step "away: paired from the app; the phone is not connected"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" hold --status 82,1,wifi,3 \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected OnePlus 12\" ]]"
  magick -size 96x96 gradient:"#40566a-#c9a46a" "$RUNTIME/art.png"
  say "media {\"player\":\"spotify\",\"name\":\"Spotify\",\"state\":\"playing\",\"title\":\"Kathmandu Rain\",\"artist\":\"Bipul Chettri\",\"album\":\"Rithu\",\"length_ms\":220000,\"position_ms\":72000,\"volume\":70,\"can\":[\"play\",\"pause\",\"play-pause\"],\"artwork_file\":\"$RUNTIME/art.png\"}"
  wait_for "no MPRIS player for the phone" eval "playerctl -l 2> /dev/null | grep -qx andromeda_link_$ID"
  shot player
  step "player: the phone plays; shot before any notification covers the header"
  say "notify {\"id\":\"mail-1\",\"app\":\"Gmail\",\"title\":\"Invoice INV-2026-1043\",\"text\":\"Your payment of Rs 13,000 is due on Friday\"}"
  say "call {\"state\":\"ringing\",\"number\":\"+977 980-1234567\",\"name\":\"Bishal Rai\"}"
  sleep 0.5 # real time: the ringing row shows before the call ends unanswered
  say "call {\"state\":\"idle\"}"
  say "notify {\"id\":\"chat-1\",\"app\":\"WhatsApp\",\"title\":\"Ada\",\"text\":\"Are we still on for 6? I can bring the projector\",\"actions\":[{\"id\":\"0\",\"label\":\"Reply\",\"reply\":true}]}"
  mkdir -p "$RUNTIME/src"
  head -c 4200000 /dev/urandom > "$RUNTIME/src/IMG_2201.jpg"
  say "send $RUNTIME/src/IMG_2201.jpg"

  sleep 1 # real time: the app gives the composer the focus when the phone connects
  key "See you at 6"
  key -k Return
  wait_for "the phone never got the composer text" grep -q "\"event\":\"received\".*\"text\":\"See you at 6\"" "$OUT/hold.jsonl"
  step "composer: $(grep "\"event\":\"received\"" "$OUT/hold.jsonl" | tail -1)"

  printf "INV-2026-1043, Rs 13,000" | wl-copy
  wait_for "link-session never synced the copy" eval "(( \$(clips) >= 1 ))"
  before=$(clips)
  # GTK does not match wtype modifiers to shortcuts, so Send clipboard runs through GTK actions on D-Bus.
  gdbus call --session --dest org.andromeda.Link --object-path /org/andromeda/Link/window/1 \
    --method org.gtk.Actions.Activate send-clipboard "[]" "{}" > /dev/null || fail "no send-clipboard action"
  wait_for "Send clipboard did not reach the phone" eval "(( \$(clips) > before ))"
  grep "\"event\":\"clip-offered\"" "$OUT/hold.jsonl" | tail -1 | grep -q "INV-2026-1043" || fail "the clip had other text"
  step "send clipboard: $(clips) offers, the last from the button"
  shot light

  kill $app_pid
  app prefer-dark
  shot dark
  step "dark: the same phone, restarted app (history starts empty)"
'
echo "PASS; artifacts: $OUT"
