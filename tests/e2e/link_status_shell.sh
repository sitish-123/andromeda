#!/usr/bin/env bash
# Phone status through the shell: a present headless phone reporting 42%, charging, Wi-Fi shows in the Devices tab and
# as the bar's phone indicator (battery glyph and percent), which updates when the status changes and disappears when
# the phone disconnects, leaving the bar as it was. Writes status-devices.png, status-changed.png, status-gone.png,
# bar-diff.json, steps.txt, hold.jsonl, linkd.log, and phone.log to $OUT
# (default ./artifacts/link-status-shell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-status-shell}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl"

with_noctalia '
  BIN='"$LINK_BIN"'
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
  sleep 1 # real time: repaint
  grim "$OUT/status-before.png"

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold --status 42,1,wifi \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "no status on D-Bus" eval "prop DeviceStatus | grep -qF \"(uint32 42, true, '"'"'wifi'"'"')\""
  sleep 1.5 # real time: the bar and the Devices tab repaint
  grim "$OUT/status-devices.png"
  step "status: $(prop DeviceStatus)"

  say "status 7 0 cellular"
  wait_for "the changed status never arrived" eval "prop DeviceStatus | grep -qF \"(uint32 7, false, '"'"'cellular'"'"')\""
  sleep 1.5 # real time: repaint
  grim "$OUT/status-changed.png"
  step "changed: $(prop DeviceStatus)"

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still connected after it stopped" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"
  sleep 1.5 # real time: repaint
  grim "$OUT/status-gone.png"
  python3 - "$OUT/status-before.png" "$OUT/status-devices.png" "$OUT/status-gone.png" "$OUT/status-changed.png" > "$OUT/bar-diff.json" <<PY
import json, sys
from PIL import Image, ImageChops
before, shown, gone, changed = (Image.open(p).convert("RGB") for p in sys.argv[1:5])
w = before.size[0]
# From the notification bell rightwards, clear of the media label, whose text scrolls.
bar = lambda im: im.crop((w - 280, 0, w, 40))
diff = lambda a, b: ImageChops.difference(bar(a), bar(b)).getbbox() is not None
json.dump({"bar_changed_when_shown": diff(before, shown), "bar_changed_on_status": diff(shown, changed),
           "bar_restored_when_gone": not diff(before, gone)}, sys.stdout)
PY
  step "bar: $(cat "$OUT/bar-diff.json")"
  grep -q "\"bar_changed_when_shown\": true" "$OUT/bar-diff.json" || fail "the indicator did not appear"
  grep -q "\"bar_changed_on_status\": true" "$OUT/bar-diff.json" || fail "the indicator did not follow the status"
  grep -q "\"bar_restored_when_gone\": true" "$OUT/bar-diff.json" || fail "the indicator outlived the connection"
'
cat "$OUT/steps.txt"
for shot in status-devices status-changed status-gone; do [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }; done
echo "PASS; artifacts: $OUT"
