#!/usr/bin/env bash
# Texts through the shell, driven by a virtual pointer and keyboard. A present headless phone with its messages switch
# on and three conversations: the Devices tab shows a texts button on the phone's row; it opens the conversations,
# newest first, named from the phone's contacts; a click opens one, oldest to newest; a text typed and sent there
# reaches the phone (its text-sent line) and shows in the conversation once the phone reports the change; a send the
# phone fails shows its reason. Writes texts-list.png, texts-thread.png, texts-sent.png, texts-failed.png, steps.txt,
# hold.jsonl, linkd.log, and phone.log to $OUT (default ./artifacts/link-texts-shell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-texts-shell}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
POINTER=${POINTER:-$(dirname "$UMBRIEL")/tests/pointer-client}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl"

with_noctalia '
  # A failure exits before with_noctalia stops the jobs, so the headless phone would outlive the run.
  trap "kill \$(jobs -p) 2>/dev/null || true" EXIT
  BIN='"$LINK_BIN"'
  POINTER='"$POINTER"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 200); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  say() { printf "%s\n" "$*" >&3; }
  prop() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 "$1"; }
  # The pointer leaves after each click so nothing paints hovered.
  click() { "$POINTER" 1280 720 move "$1" "$2" click 272 move 5 715 > /dev/null; }
  # wtype drops a key sent right after it uploads its keymap, so it waits first.
  type_into() { click "$1" "$2"; wtype -s 300 "$3"; }
  shot() { sleep 1.5; grim "$OUT/$1.png"; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw umbriel-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/umbriel-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
    >> "$OUT/phone.log" 2>&1 || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  wait_for "the pairing session never ended" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"
  python3 - "$RUNTIME/phone/devices.json" "$RUNTIME/texts.json" "$RUNTIME/contacts.json" <<PY
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["messages"] = True
json.dump(store, open(sys.argv[1], "w"))
now = 1_790_000_000_000
texts = [
    {"id": 6, "thread": 1, "address": "+9779800000001", "body": "See you at six?", "date": now, "sent": False},
    {"id": 5, "thread": 2, "address": "+9779800000002", "body": "Parcel delivered", "date": now - 3_600_000, "sent": False},
    {"id": 4, "thread": 1, "address": "+9779800000001", "body": "On my way", "date": now - 7_200_000, "sent": True},
    {"id": 3, "thread": 3, "address": "NTC", "body": "Your balance is low", "date": now - 86_400_000, "sent": False},
    {"id": 2, "thread": 1, "address": "+9779800000001", "body": "Lunch tomorrow?", "date": now - 90_000_000, "sent": False},
]
json.dump(texts, open(sys.argv[2], "w"))
json.dump([{"name": "Asha", "numbers": ["9800000001"]}, {"name": "Courier", "numbers": ["+977 980 000 0002"]}], open(sys.argv[3], "w"))
PY

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --texts "$RUNTIME/texts.json" \
    --contacts "$RUNTIME/contacts.json" hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "no texts on D-Bus" eval "prop Texts | grep -qF \"See you at six\""
  msg panel-open control-center devices > /dev/null
  shot devices
  # The texts button on the phone row, then the first conversation, at 1280x720 and 1x scale.
  click 708 273
  shot texts-list
  step "conversations: $(prop Texts | grep -o "See you at six" | head -1), named from contacts"
  click 478 358
  shot texts-thread
  step "conversation: Asha (+9779800000001), three texts oldest first"

  type_into 647 469 "Running ten minutes late"
  click 868 469
  wait_for "the phone never sent the text" grep -q "\"event\":\"text-sent\"" "$OUT/hold.jsonl"
  grep -q "\"to\":\"+9779800000001\"" "$OUT/hold.jsonl" || fail "the text went to another number: $(cat "$OUT/hold.jsonl")"
  say texts-changed
  wait_for "the sent text never reached Texts" eval "prop Texts | grep -qF \"Running ten minutes late\""
  shot texts-sent
  step "sent from the conversation: $(grep text-sent "$OUT/hold.jsonl")"

  click 466 355
  sleep 0.5
  type_into 667 484 fail
  type_into 647 526 "No service here"
  click 868 526
  wait_for "the shell never logged the failed send" grep -q "SendText" "$OUT/noctalia.log"
  shot texts-failed
  step "a send the phone fails: $(grep SendText "$OUT/noctalia.log" | tail -1 | sed "s/.*SendText/SendText/")"
'
cat "$OUT/steps.txt"
for shot in texts-list texts-thread texts-sent texts-failed; do [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }; done
echo "PASS; artifacts: $OUT"
