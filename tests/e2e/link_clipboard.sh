#!/usr/bin/env bash
# Clipboard sync through andromeda-link-session's ext-data-control-v1 backend, beside the shell's clipboard history,
# against a real andromeda-linkd and a present headless phone. Proves: a desktop copy (wl-copy) reaches the phone at once as inline text; a phone offer becomes the desktop selection without moving any
# bytes (no clip-pull or clip-data in the transcript) until wl-paste pulls it, as text and as a PNG, byte for byte;
# the same text copied twice crosses twice, either way; a phone clip of text and HTML pastes each type's own bytes,
# and desktop HTML and PNG copies reach the phone byte for byte; a desktop copy made while the phone is away reaches
# it when it connects; the desktop offers nothing back after a paste; with the clipboard grant off nothing crosses
# either way, and with the files grant off an offer of files is declined. Every message is validated against link/api/messages.cddl. Writes steps.txt,
# counts.json, transcript.jsonl, signals.txt, hold.jsonl, linkd.log, session.log, and phone.log to $OUT
# (default ./artifacts/link-clipboard).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-clipboard}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.txt "$OUT"/*.jsonl "$OUT/counts.json"
# PROTOCOL=wlr runs the wlr-data-control fallback instead.
PROTOCOL=${PROTOCOL:-ext}
BACKEND=$([[ $PROTOCOL == wlr ]] && echo wlr-data-control-unstable-v1 || echo ext-data-control-v1)

with_noctalia '
  BIN='"$LINK_BIN"'
  SCHEMA='"$ROOT"'/link/api/messages.cddl
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  say() { printf "%s\n" "$*" >&3; }
  link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
  count() { grep -c "\"cbor\":\"[0-9a-f]*$(printf %s "$1" | xxd -p)" "$OUT/transcript.jsonl" || true; }
  offered_to_phone() { grep -c "\"event\":\"clip-offered\"" "$OUT/hold.jsonl" || true; }
  offered_to_desktop() { grep -c "member=ClipboardOffered" "$OUT/signals.txt" || true; }
  mkdir -p "$RUNTIME/src"

  XDG_STATE_HOME=$RUNTIME/linkd XDG_DOWNLOAD_DIR=$RUNTIME/downloads "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  ANDROMEDA_LINK_DATA_CONTROL='"$PROTOCOL"' "$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
  wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
  grep -q "clipboard through '"$BACKEND"'" "$OUT/session.log" || fail "link-session has no '"$BACKEND"' backend"
  dbus-monitor --session "type='"'"'signal'"'"',interface='"'"'org.andromeda.Link1'"'"'" > "$OUT/signals.txt" 2>&1 &
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
    >> "$OUT/phone.log" 2>&1 || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  GRANTS=$(gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 Grants)
  [[ $GRANTS == *clipboard*files*notifications*media*ring*calls* ]] || fail "default grants: $GRANTS"
  step "default grants: $GRANTS"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  hold() {
    "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
      --downloads "$RUNTIME/phone-downloads" hold < "$RUNTIME/phone.in" >> "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
    HOLD=$!
    wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  }
  last_clip() { grep "\"event\":\"clip-offered\"" "$OUT/hold.jsonl" | tail -1 | python3 -c "import json,sys; print(json.load(sys.stdin)[\"clip\"])"; }
  pulled() { grep -c "\"event\":\"pulled\"" "$OUT/hold.jsonl" || true; }
  pull() {
    local before; before=$(pulled)
    say "pull $(last_clip) $1 $2"
    wait_for "the phone never pulled $1" eval "(( \$(pulled) > $before ))"
  }
  hold

  printf "copied on the desktop" | wl-copy
  wait_for "the phone never got the desktop copy" grep -q "\"text\":\"copied on the desktop\"" "$OUT/hold.jsonl"
  step "desktop copy reached the phone inline: $(offered_to_phone) offer"
  printf "copied on the desktop" | wl-copy
  wait_for "the same text copied again never reached the phone" eval "(( \$(offered_to_phone) == 2 ))"
  step "the same text copied twice on the desktop reached the phone twice"

  # The headless phone never sets its clipboard, so this is a new copy of the same text, not an echo.
  printf "copied on the desktop" > "$RUNTIME/src/echo.txt"
  BEFORE=$(offered_to_desktop)
  say "clip $RUNTIME/src/echo.txt text/plain;charset=utf-8 text/plain"
  wait_for "the phone copy of the same text never reached the desktop" eval "(( \$(offered_to_desktop) > $BEFORE ))"
  wait_for "clipboard history never read the phone copy" eval "(( \$(count clip-data) == 1 ))"
  step "the phone copying the text it got is a new copy: it reached the desktop"
  BEFORE=$(offered_to_desktop)

  printf "copied on the phone" > "$RUNTIME/src/phone.txt"
  say "clip $RUNTIME/src/phone.txt text/plain;charset=utf-8 text/plain"
  wait_for "no ClipboardOffered for the phone text" eval "(( \$(offered_to_desktop) > $BEFORE ))"
  sleep 0.5 # real time: the shell takes the selection
  PULLS_BEFORE=$(count clip-pull); DATA_BEFORE=$(count clip-data)
  # Clipboard history reads text from the phone, which is small, as soon as it is offered.
  [[ $PULLS_BEFORE == 2 && $DATA_BEFORE == 2 ]] || fail "history read moved $PULLS_BEFORE pulls, $DATA_BEFORE streams"
  PHONE_OFFERS=$(offered_to_phone)
  [[ $(wl-paste -n -t "text/plain;charset=utf-8") == "copied on the phone" ]] || fail "wl-paste got $(wl-paste -n)"
  PULLS_AFTER=$(count clip-pull); DATA_AFTER=$(count clip-data)
  [[ $PULLS_AFTER == 3 && $DATA_AFTER == 3 ]] || fail "one paste moved $PULLS_AFTER pulls and $DATA_AFTER streams"
  sleep 1 # real time: long enough for a clip-offer the desktop should not send back
  [[ $(offered_to_phone) == "$PHONE_OFFERS" ]] || fail "the desktop echoed the phone clip back"
  step "phone text pulled only on paste (pulls $PULLS_BEFORE to $PULLS_AFTER, streams $DATA_BEFORE to $DATA_AFTER), no echo"
  # A clipboard manager (Klipper) takes the selection of the phone clip again, without the remote type.
  BEFORE=$(offered_to_desktop)
  say "clip-text kept by a clipboard manager"
  wait_for "no ClipboardOffered for the inline phone text" eval "(( \$(offered_to_desktop) > $BEFORE ))"
  sleep 0.5 # real time: the shell takes the selection
  [[ $(wl-paste -n -t "text/plain;charset=utf-8") == "kept by a clipboard manager" ]] || fail "wl-paste got $(wl-paste -n)"
  wl-paste -n -t "text/plain;charset=utf-8" | wl-copy
  sleep 1 # real time: long enough for a clip-offer the desktop should not send back
  [[ $(offered_to_phone) == "$PHONE_OFFERS" ]] || fail "a clipboard manager taking the phone clip sent it back"
  step "a clipboard manager taking the phone clip did not send it back"
  DATA_AFTER=$(count clip-data)

  grim "$RUNTIME/src/shot.png"
  say "clip $RUNTIME/src/shot.png image/png"
  wait_for "no ClipboardOffered for the phone image" eval "(( \$(offered_to_desktop) > $BEFORE + 1 ))"
  sleep 0.5 # real time: the shell takes the selection
  [[ $(count clip-data) == "$DATA_AFTER" ]] || fail "the image moved before the paste"
  wl-paste -t image/png > "$RUNTIME/pasted.png"
  cmp -s "$RUNTIME/src/shot.png" "$RUNTIME/pasted.png" || fail "the pasted image differs"
  IMAGE_BYTES=$(stat -c %s "$RUNTIME/pasted.png")
  step "phone image pulled on paste: $IMAGE_BYTES bytes, identical"

  printf "plain on the phone" > "$RUNTIME/src/rich.txt"
  printf "<b>bold</b> on the phone" > "$RUNTIME/src/rich.html"
  BEFORE=$(offered_to_desktop)
  say "clip $RUNTIME/src/rich.txt text/plain;charset=utf-8 text/plain + $RUNTIME/src/rich.html text/html"
  wait_for "no ClipboardOffered for the phone HTML" eval "(( \$(offered_to_desktop) > $BEFORE ))"
  sleep 0.5 # real time: the shell takes the selection
  [[ $(wl-paste -n -t text/html) == "<b>bold</b> on the phone" ]] || fail "text/html pasted $(wl-paste -n -t text/html)"
  [[ $(wl-paste -n -t "text/plain;charset=utf-8") == "plain on the phone" ]] || fail "text/plain pasted the wrong type"
  step "phone text and HTML clip: each type pasted its own bytes"

  printf "<i>desktop</i> HTML" | wl-copy -t text/html
  wait_for "the desktop HTML never reached the phone" eval "grep \"clip-offered\" \"$OUT/hold.jsonl\" | tail -1 | grep -q text/html"
  pull text/html "$RUNTIME/phone.html"
  [[ $(cat "$RUNTIME/phone.html") == "<i>desktop</i> HTML" ]] || fail "the phone pulled $(cat "$RUNTIME/phone.html")"
  wl-copy -t image/png < "$RUNTIME/src/shot.png"
  wait_for "the desktop image never reached the phone" eval "grep \"clip-offered\" \"$OUT/hold.jsonl\" | tail -1 | grep -q image/png"
  pull image/png "$RUNTIME/phone.png"
  cmp -s "$RUNTIME/src/shot.png" "$RUNTIME/phone.png" || fail "the image the phone pulled differs"
  step "desktop HTML and PNG copies reached the phone byte for byte"

  kill "$HOLD"; wait "$HOLD" || true
  wait_for "phone still shown connected" eval "[[ \$(msg link-devices) != \"$ID connected E2E Phone\" ]]"
  OFFERS=$(offered_to_phone)
  printf "copied while the phone was away" | wl-copy
  sleep 0.5 # real time: link-session gives the copy to the daemon
  hold
  wait_for "the copy made while away never reached the phone" grep -q "\"text\":\"copied while the phone was away\"" "$OUT/hold.jsonl"
  step "a desktop copy made while the phone was away reached it on connect"
  python3 -c "import json,sys; json.dump({\"before_paste\":{\"clip_pull\":int(sys.argv[1]),\"clip_data\":int(sys.argv[2])},\"after_text_paste\":{\"clip_pull\":int(sys.argv[3]),\"clip_data\":int(sys.argv[4])},\"image_bytes\":int(sys.argv[5])}, open(sys.argv[6],\"w\"), indent=2)" \
    "$PULLS_BEFORE" "$DATA_BEFORE" "$PULLS_AFTER" "$DATA_AFTER" "$IMAGE_BYTES" "$OUT/counts.json"

  link SetGrant "$ID" clipboard false > /dev/null || fail "SetGrant clipboard"
  wait_for "Grants did not drop the clipboard" eval "gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 Grants | grep -vq clipboard"
  PHONE_OFFERS=$(offered_to_phone); DESKTOP_OFFERS=$(offered_to_desktop)
  printf "not for the phone" | wl-copy
  printf "not for the desktop" > "$RUNTIME/src/denied.txt"
  say "clip $RUNTIME/src/denied.txt text/plain"
  sleep 1.5 # real time: long enough for offers that must not arrive
  [[ $(offered_to_phone) == "$PHONE_OFFERS" ]] || fail "a desktop copy reached a phone without the grant"
  [[ $(offered_to_desktop) == "$DESKTOP_OFFERS" ]] || fail "a phone offer reached D-Bus without the grant"
  grep -q "no clipboard grant" "$OUT/linkd.log" || fail "the daemon did not log the dropped offer"
  step "clipboard grant off: nothing crossed either way"

  link SetGrant "$ID" files false > /dev/null || fail "SetGrant files"
  say "send $RUNTIME/src/phone.txt"
  wait_for "a file offer without the files grant was not declined" grep -q "\"status\":\"declined\"" "$OUT/hold.jsonl"
  step "files grant off: the offer was declined"

  "$BIN/andromeda-link-phone" check-transcript "$SCHEMA" "$OUT/transcript.jsonl" > "$OUT/schema.txt" \
    || fail "transcript violates the schema: $(cat "$OUT/schema.txt")"
  step "transcript: $(cat "$OUT/schema.txt")"
'
cat "$OUT/steps.txt"
echo "PASS; artifacts: $OUT"
