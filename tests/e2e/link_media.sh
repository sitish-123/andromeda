#!/usr/bin/env bash
# Media both ways, through the shell, against a real umbriel-linkd and a present headless phone. Proves: a track
# playing on the phone appears as the MPRIS player umbriel_link_<device> that playerctl reads (title, artist, status,
# position, volume, artwork file) and that the shell's bar, media tab, and lock screen show with no shell change;
# playerctl's play-pause, seek, volume, and next reach the phone as media-command, and the phone's new state comes
# back; the player goes when the phone stops it or the session ends. The reverse: a desktop MPRIS test player is sent
# to the phone as media-player (with its state, commands, and volume), a phone command pauses it, seeks it, sets its
# volume, and skips it, each landing on the player and its new state reaching the phone. Every message is validated
# against protocol/link-v1/messages.cddl. Writes media-*.png, steps.txt, hold.jsonl, player-calls.txt, transcript.jsonl,
# linkd.log, and noctalia.log to $OUT (default ./artifacts/link-media).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-media}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
boot_headless 1

python3 - "$RUNTIME/art.png" <<'PY'
import struct, sys, zlib
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
side = 128
rows = b"".join(b"\0" + b"".join(bytes((x * 2, 40, 255 - x * 2, 255)) for x in range(side)) for _ in range(side))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY

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
  pc() { playerctl -p "$PLAYER" "$@" 2> /dev/null || true; }
  phone_saw() { grep -q "$1" "$OUT/hold.jsonl"; }
  calls() { grep -qx "$1" "$OUT/player-calls.txt" 2> /dev/null; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  PLAYER=umbriel_link_$ID

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
    hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"

  say "media {\"player\":\"spotify\",\"name\":\"Spotify\",\"state\":\"playing\",\"title\":\"Night Drive\",\"artist\":\"Kavinsky\",\"album\":\"OutRun\",\"length_ms\":240000,\"position_ms\":30000,\"volume\":70,\"can\":[\"play\",\"pause\",\"play-pause\",\"next\",\"previous\",\"seek\",\"volume\"],\"artwork_file\":\"$RUNTIME/art.png\"}"
  wait_for "no MPRIS player $PLAYER: $(playerctl -l 2>&1)" eval "playerctl -l 2> /dev/null | grep -qx \"$PLAYER\""
  wait_for "the MPRIS title is wrong: $(pc metadata title)" eval "[[ \$(pc metadata title) == \"Night Drive\" ]]"
  [[ $(pc metadata artist) == Kavinsky && $(pc status) == Playing ]] || fail "artist or status: $(pc metadata artist) $(pc status)"
  [[ $(pc volume) == 0.7* ]] || fail "volume: $(pc volume)"
  ART=$(pc metadata mpris:artUrl)
  [[ $ART == file://*.png && -s ${ART#file://} ]] || fail "no artwork file: $ART"
  cmp -s "${ART#file://}" "$RUNTIME/art.png" || fail "the artwork file differs from what the phone sent"
  POS=$(pc position | cut -d. -f1)
  (( POS >= 30 && POS < 40 )) || fail "position does not advance from 30 s: $POS"
  step "phone player on MPRIS: $(pc metadata --format "{{title}} by {{artist}}, {{status}}, {{position}} us, volume {{volume}}") art $ART"
  sleep 1.5 # real time: the bar and media tab repaint
  grim "$OUT/media-bar.png"
  msg panel-open control-center media > /dev/null
  sleep 1.5 # real time: the panel opens and paints the player
  grim "$OUT/media-tab.png"
  msg panel-close > /dev/null

  pc play-pause
  wait_for "phone did not get play-pause" phone_saw "\"command\":\"play-pause\".*\"event\":\"media-command\""
  wait_for "MPRIS does not show the phone paused: $(pc status)" eval "[[ \$(pc status) == Paused ]]"
  pc position 100
  wait_for "phone did not get the seek" phone_saw "\"command\":\"seek\".*\"value\":100000"
  pc volume 0.4
  wait_for "phone did not get the volume" phone_saw "\"command\":\"volume\".*\"value\":40"
  wait_for "MPRIS volume did not follow the phone: $(pc volume)" eval "[[ \$(pc volume) == 0.4* ]]"
  pc next
  wait_for "phone did not get next" phone_saw "\"command\":\"next\""
  wait_for "the new track did not reach MPRIS: $(pc metadata title)" eval "[[ \$(pc metadata title) == *Next* ]]"
  step "desktop to phone player: play-pause, seek 100 s, volume 0.4, next; MPRIS now $(pc status) $(pc metadata title)"

  msg session lock > /dev/null
  sleep 2 # real time: the lock surface maps and paints its media card
  grim "$OUT/media-lock-screen.png"
  step "lock screen screenshot taken while the phone player is paused"

  python3 "$E2E/mpris_test_player.py" "$OUT/player-calls.txt" &
  wait_for "the desktop player never reached the phone" phone_saw "\"event\":\"media-player\".*\"player\":\"e2e\".*\"title\":\"Desk Session\""
  grep "\"player\":\"e2e\"" "$OUT/hold.jsonl" | tail -1 | grep -q "\"state\":\"playing\"" || fail "the desktop player is not playing on the phone"
  grep "\"player\":\"e2e\"" "$OUT/hold.jsonl" | tail -1 | grep -q "\"volume\":50" || fail "the desktop volume did not reach the phone"
  grep -q "\"player\":\"$PLAYER\"\|\"player\":\"umbriel_link" "$OUT/hold.jsonl" && fail "the daemon sent the phone its own exported player"
  step "desktop player on the phone: $(grep "\"player\":\"e2e\"" "$OUT/hold.jsonl" | tail -1)"

  say "media-command {\"player\":\"e2e\",\"command\":\"pause\"}"
  wait_for "the desktop player was not paused" calls Pause
  wait_for "the paused state did not reach the phone" eval "grep \"\\\"player\\\":\\\"e2e\\\"\" \"$OUT/hold.jsonl\" | tail -1 | grep -q paused"
  say "media-command {\"player\":\"e2e\",\"command\":\"seek\",\"value\":42000}"
  wait_for "the desktop player was not seeked" calls "SetPosition /org/e2e/track/1 42000000"
  wait_for "the seek did not reach the phone" phone_saw "\"player\":\"e2e\",\"position_ms\":42000"
  say "media-command {\"player\":\"e2e\",\"command\":\"volume\",\"value\":25}"
  wait_for "the desktop volume was not set" calls "Set Volume 0.25"
  say "media-command {\"player\":\"e2e\",\"command\":\"next\"}"
  wait_for "the desktop player did not skip" calls Next
  wait_for "the next track did not reach the phone" phone_saw "\"title\":\"Desk Session, part 2\""
  step "phone to desktop player: $(tr "\n" " " < "$OUT/player-calls.txt")"

  say "media-gone spotify"
  wait_for "the phone player outlived media-gone" eval "! playerctl -l 2> /dev/null | grep -qx \"$PLAYER\""
  [[ -e ${ART#file://} ]] && fail "the artwork file outlived its player"
  say "media {\"player\":\"spotify\",\"name\":\"Spotify\",\"state\":\"playing\",\"title\":\"Again\",\"artist\":\"\",\"album\":\"\",\"position_ms\":0,\"can\":[\"play-pause\"]}"
  wait_for "the player did not return" eval "playerctl -l 2> /dev/null | grep -qx \"$PLAYER\""
  kill "$hold"
  wait "$hold" || true
  wait_for "the phone player outlived its session" eval "! playerctl -l 2> /dev/null | grep -qx \"$PLAYER\""
  step "phone player gone after media-gone and after the session ended"
'
cat "$OUT/steps.txt"
BIN=${LINK_BIN:-$ROOT/link/target/debug}
"$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl" \
  | tee "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
for shot in media-bar media-tab media-lock-screen; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
