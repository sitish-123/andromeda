#!/usr/bin/env bash
# Find my phone and find my desktop, through the shell, against a real umbriel-linkd and a present headless phone,
# with a private PipeWire whose null sink is recorded. Proves: link-ring rings the phone, whose ringing report turns
# the Devices tab's Ring button into Stop, and link-ring stop stops it; a phone that stops ringing by itself is shown
# stopped; the phone rings the desktop, which plays the alarm sound at full volume although UI sounds are off (the
# recording of the sink is loud), shows a Stop notification, and reports ringing to the phone; Stop on the desktop
# silences it and tells the phone, and the phone can stop the desktop too. Every message is validated against
# protocol/link-v1/messages.cddl. Writes ring-*.png, ring.wav, volume.txt, steps.txt, hold.jsonl, transcript.jsonl,
# linkd.log, and noctalia.log to $OUT (default ./artifacts/link-ring).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-ring}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
boot_headless 1

# PipeWire before the shell, which opens its sound player at start; off the real session bus.
mkdir -p "$RUNTIME/pw-config"
run env -u DBUS_SESSION_BUS_ADDRESS XDG_CONFIG_HOME="$RUNTIME/pw-config" pipewire > "$OUT/pipewire.log" 2>&1 &
for _ in $(seq 50); do [[ -S $RUNTIME/pipewire-0 ]] && break; sleep 0.1; done
run env -u DBUS_SESSION_BUS_ADDRESS XDG_CONFIG_HOME="$RUNTIME/pw-config" wireplumber > "$OUT/wireplumber.log" 2>&1 &
sleep 1 # real time: WirePlumber connects and loads its policy
run pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=e2e-sink media.class=Audio/Sink object.linger=true audio.position=[FL FR] }" > /dev/null
for _ in $(seq 50); do run wpctl status 2> /dev/null | grep -q e2e-sink && break; sleep 0.1; done
sink=$(run pw-cli ls Node | awk "/^\tid / {id = \$2} /node.name = \"e2e-sink\"/ {print id}" | tr -d ,)
run wpctl set-default "$sink"

with_noctalia '
  BIN='"$LINK_BIN"'
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
  phone_saw() { [[ $(grep -c "$1" "$OUT/hold.jsonl") -ge ${2:-1} ]]; }
  ringing() { [[ $(msg link-ringing) == "$1" ]]; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  [[ $(msg link-ring "$ID") == *"is not connected"* ]] || fail "link-ring to a disconnected phone: $(msg link-ring "$ID")"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
    hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  msg panel-open control-center devices > /dev/null

  [[ $(msg link-ring "$ID") == ok ]] || fail "link-ring"
  wait_for "the phone was not rung" phone_saw "\"event\":\"ring\",\"on\":true"
  wait_for "the shell does not show the phone ringing: $(msg link-ringing)" ringing "phone $ID"
  sleep 1 # real time: the tab repaints its Stop button
  grim "$OUT/ring-phone-ringing.png"
  [[ $(msg link-ring "$ID" stop) == ok ]] || fail "link-ring stop"
  wait_for "the phone was not stopped" phone_saw "\"event\":\"ring\",\"on\":false"
  wait_for "the shell still shows the phone ringing" ringing none
  step "desktop rang the phone and stopped it; ringing reports followed"

  [[ $(msg link-ring "$ID") == ok ]] || fail "link-ring again"
  wait_for "the shell does not show the second ring" ringing "phone $ID"
  say "ringing off"
  wait_for "a phone stopped on the phone still shows ringing" ringing none
  step "the phone stopped its own ring; the shell followed its report"
  msg panel-close > /dev/null

  pw-record -P "{ stream.capture.sink=true }" --target e2e-sink "$OUT/ring.wav" &
  recorder=$!
  sleep 0.5 # real time: the recorder links to the sink
  say "ring on"
  wait_for "the desktop did not ring: $(msg link-ringing)" ringing "desktop $ID"
  wait_for "the phone did not hear the desktop ringing" phone_saw "\"event\":\"desktop-ringing\".*\"on\":true"
  sleep 3 # real time: the alarm plays more than once
  grim "$OUT/ring-desktop-ringing.png"
  [[ $(msg notification-invoke-latest) == ok ]] || fail "pressing Stop"
  wait_for "Stop did not silence the desktop" ringing none
  wait_for "the phone did not hear the desktop stop" phone_saw "\"event\":\"desktop-ringing\".*\"on\":false"
  kill "$recorder"
  wait "$recorder" || true
  step "phone rang the desktop, Stop on the desktop silenced it and told the phone"

  say "ring on"
  wait_for "the desktop did not ring again" ringing "desktop $ID"
  say "ring off"
  wait_for "the phone could not stop the desktop" ringing none
  wait_for "the phone did not hear the second stop" phone_saw "\"event\":\"desktop-ringing\".*\"on\":false" 2
  step "the phone stopped the desktop ringing"
  kill "$hold"
  wait "$hold" || true
'
cat "$OUT/steps.txt"
ffmpeg -hide_banner -i "$OUT/ring.wav" -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume" | tee "$OUT/volume.txt"
max=$(sed -n "s/.*max_volume: \(-\?[0-9.]*\) dB/\1/p" "$OUT/volume.txt")
awk -v m="$max" 'BEGIN {exit !(m > -12)}' || { echo "FAIL: the ring is not loud (max $max dB)" >&2; exit 1; }
BIN=${LINK_BIN:-$ROOT/link/target/debug}
"$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl" \
  | tee "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
for shot in ring-phone-ringing ring-desktop-ringing; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
