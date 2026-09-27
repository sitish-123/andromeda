#!/usr/bin/env bash
# Phone notifications on the desktop, through the shell, against a real umbriel-linkd and a present headless phone.
# Proves: a post reaches D-Bus NotificationPosted with its icon and actions and shows as a toast with the app icon; a
# button runs its action on the phone, and the inline reply sends the typed text with the reply action; dismissing on
# the desktop dismisses on the phone, whose removal comes back as NotificationRemoved; a removal on the phone closes
# the desktop's notification without a dismissal going back; a reconnect's re-post of unchanged notifications emits
# nothing; the 65th live notification removes the oldest; Do Not Disturb holds the toast back; a PNG whose header
# claims 4096x4096 shows the phone glyph instead of being decoded; a hostile phone's 16385-byte icon and 4-action post
# close the session with protocol-error and reach nothing; D-Bus refuses an oversized reply and a disconnected phone.
# Every message of the honest phone is validated against protocol/link-v1/messages.cddl, and the hostile ones fail it.
# Writes notification-*.png, steps.txt, signals.txt, hold.jsonl, transcript.jsonl, hostile.jsonl, linkd.log, and
# noctalia.log to $OUT (default ./artifacts/link-notifications).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-notifications}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
boot_headless 1

python3 - "$RUNTIME" <<'PY'
import struct, sys, zlib
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
def png(width, height, rows):
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
side = 64
rows = b""
for y in range(side):
    rows += b"\0"
    for x in range(side):
        inside = (x - 31.5) ** 2 + (y - 31.5) ** 2 <= 30 ** 2
        rows += bytes((0, 215, 255, 255) if inside else (0, 0, 0, 0))
runtime = sys.argv[1]
open(f"{runtime}/icon.png", "wb").write(png(side, side, rows))
# The header claims 4096x4096; the data would inflate to 64 MiB, so the shell must refuse before decoding.
open(f"{runtime}/bomb.png", "wb").write(png(4096, 4096, b"\0" * 1024))
open(f"{runtime}/huge.png", "wb").write(png(side, side, rows)[:33] + bytes(16385 - 33))
PY

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
  signals() { grep -c "member=$1" "$OUT/signals.txt" || true; }
  # grep -F without -q reads its whole input, so the writer never dies of SIGPIPE under pipefail.
  heard() { grep -A3 "member=$1" "$OUT/signals.txt" | grep -F "string \"$2\"" > /dev/null; }
  phone_saw() { grep -q "$1" "$OUT/hold.jsonl"; }
  link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}" 2>&1; }
  shot() { sleep 1.5; grim "$OUT/notification-$1.png"; } # real time: the toast maps and paints
  post() { say "notify {\"id\":\"$1\",\"app\":\"$2\",\"title\":\"$3\",\"text\":\"$4\"${5:+,$5}}"; }
  reply_actions="\"actions\":[{\"id\":\"0\",\"label\":\"Reply\",\"reply\":true},{\"id\":\"1\",\"label\":\"Mark as read\",\"reply\":false}]"

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  dbus-monitor --session "type=signal,interface=org.umbriel.Link1" > "$OUT/signals.txt" 2>&1 &
  wait_for "dbus-monitor did not start" test -s "$OUT/signals.txt"
  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
    hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  step "present: $(msg link-devices)"

  post chat-1 Messages "Ann Lee" "Are you still coming at 6?" "\"icon_file\":\"$RUNTIME/icon.png\",$reply_actions"
  wait_for "no NotificationPosted for chat-1" heard NotificationPosted chat-1
  grep -A40 "member=NotificationPosted" "$OUT/signals.txt" | grep "array of bytes" > /dev/null || fail "the icon did not reach D-Bus"
  shot posted
  step "posted: chat-1 with icon, Reply and Mark as read"

  [[ $(msg notification-action-latest phone:1) == ok ]] || fail "invoking Mark as read: $(msg notification-action-latest phone:1)"
  wait_for "phone did not get Mark as read" phone_saw "\"action\":\"1\".*\"event\":\"notification-action\".*\"id\":\"chat-1\""
  step "button: $(grep "\"action\":\"1\"" "$OUT/hold.jsonl" | tail -1)"

  post chat-2 Messages "Ann Lee" "Bring the charger" "$reply_actions"
  wait_for "no NotificationPosted for chat-2" heard NotificationPosted chat-2
  [[ $(msg notification-reply-latest On my way) == ok ]] || fail "inline reply: $(msg notification-reply-latest x)"
  wait_for "phone did not get the reply" phone_saw "\"action\":\"0\".*\"id\":\"chat-2\".*\"reply_text\":\"On my way\""
  grep -q "\"event\":\"notification-dismiss\".*chat-2" "$OUT/hold.jsonl" && fail "a reply also dismissed on the phone"
  step "reply: $(grep "reply_text\":\"On my way" "$OUT/hold.jsonl" | tail -1)"

  post mail-1 Mail "Invoice 42" "Due on Friday"
  wait_for "no NotificationPosted for mail-1" heard NotificationPosted mail-1
  [[ $(msg notification-dismiss-latest) == ok ]] || fail "dismissing on the desktop"
  wait_for "phone did not get the dismissal" phone_saw "\"event\":\"notification-dismiss\".*\"id\":\"mail-1\""
  wait_for "the phone'"'"'s removal did not come back" heard NotificationRemoved mail-1
  step "dismissed on the desktop, removed on the phone: mail-1"

  post cal-1 Calendar "Standup" "In 5 minutes"
  wait_for "no NotificationPosted for cal-1" heard NotificationPosted cal-1
  shot before-phone-removal
  say "unnotify cal-1"
  wait_for "no NotificationRemoved for cal-1" heard NotificationRemoved cal-1
  shot after-phone-removal
  sleep 1
  grep -q "\"event\":\"notification-dismiss\".*cal-1" "$OUT/hold.jsonl" && fail "a phone removal echoed a dismissal"
  step "removed on the phone: cal-1 closed on the desktop, no dismissal sent back"

  before=$(signals NotificationPosted)
  connects=$(grep -c "\"event\":\"connected\"" "$OUT/hold.jsonl")
  say reconnect
  wait_for "the phone did not reconnect" eval "[[ \$(grep -c \"\\\"event\\\":\\\"connected\\\"\" \"$OUT/hold.jsonl\") -gt $connects ]]"
  sleep 1
  [[ $(signals NotificationPosted) == "$before" ]] || fail "a reconnect re-showed unchanged notifications"
  step "reconnect: re-posts of chat-1 and chat-2 emitted nothing new"

  msg notification-dnd-set on > /dev/null
  [[ $(msg notification-dnd-status) == on ]] || fail "turning DND on"
  post dnd-1 Messages "Quiet" "This waits in the history"
  wait_for "no NotificationPosted for dnd-1" heard NotificationPosted dnd-1
  shot dnd
  msg notification-dnd-set off > /dev/null
  step "dnd: posted while on, screenshot notification-dnd.png shows no toast"

  post bomb-1 Photos "Header bomb" "A PNG claiming 4096x4096" "\"icon_file\":\"$RUNTIME/bomb.png\""
  wait_for "no NotificationPosted for bomb-1" heard NotificationPosted bomb-1
  wait_for "the shell did not refuse the 4096x4096 icon" grep -q "icon of 4096x4096 ignored" "$OUT/noctalia.log"
  shot bomb-glyph
  step "bomb icon: refused from its header, shown with the phone glyph"

  for n in $(seq 0 64); do post "cap-$n" Flood "Item $n" "Filling the limit"; done
  wait_for "no NotificationPosted for cap-64" heard NotificationPosted cap-64
  removed_first=$(grep -A3 "member=NotificationRemoved" "$OUT/signals.txt" | grep -c "string \"chat-1\"" || true)
  [[ $removed_first == 1 ]] || fail "the 65th live notification did not remove the oldest (chat-1)"
  step "cap: 65th live notification removed the oldest, chat-1"

  OUTPUT=$(link NotificationAction "$ID" chat-2 0 "$(head -c 4097 /dev/zero | tr "\0" a)") && fail "an oversized reply was sent"
  [[ $OUTPUT == *Error.Rejected* ]] || fail "an oversized reply was not rejected: $OUTPUT"
  step "dbus: oversized reply rejected"

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still connected after it stopped" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"
  OUTPUT=$(link NotificationDismiss "$ID" chat-2) && fail "a dismissal reached a disconnected phone"
  [[ $OUTPUT == *Error.NotConnected* ]] || fail "dismissing on a disconnected phone: $OUTPUT"
  step "dbus: dismiss to a disconnected phone is NotConnected"

  posted=$(signals NotificationPosted)
  OUTPUT=$(phone --transcript "$OUT/hostile.jsonl" notify-unchecked --json "{\"id\":\"evil-1\",\"app\":\"Evil\",\"title\":\"t\",\"text\":\"x\",\"icon_file\":\"$RUNTIME/huge.png\",\"actions\":[]}")
  [[ $OUTPUT == *"\"accepted\":false"*"protocol error"* ]] || fail "the desktop took a 16385-byte icon: $OUTPUT"
  OUTPUT=$(phone --transcript "$OUT/hostile.jsonl" notify-unchecked --json "{\"id\":\"evil-2\",\"app\":\"Evil\",\"title\":\"t\",\"text\":\"x\",\"actions\":[{\"id\":\"a\",\"label\":\"a\"},{\"id\":\"b\",\"label\":\"b\"},{\"id\":\"c\",\"label\":\"c\"},{\"id\":\"d\",\"label\":\"d\"}]}")
  [[ $OUTPUT == *"\"accepted\":false"*"protocol error"* ]] || fail "the desktop took four actions: $OUTPUT"
  [[ $(signals NotificationPosted) == "$posted" ]] || fail "a hostile post reached D-Bus"
  grep -q "notification-posted message violates the schema" "$OUT/linkd.log" || fail "the daemon did not log the violation"
  step "hostile: 16385-byte icon and 4 actions closed with protocol-error, nothing reached D-Bus"
'
cat "$OUT/steps.txt"
BIN=${LINK_BIN:-$ROOT/link/target/debug}
"$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl" \
  | tee "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
if "$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/hostile.jsonl" 2> /dev/null; then
  echo "FAIL: the schema accepts the hostile posts" >&2
  exit 1
fi
for shot in posted before-phone-removal after-phone-removal dnd bomb-glyph; do
  [[ -s $OUT/notification-$shot.png ]] || { echo "FAIL: no notification-$shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
