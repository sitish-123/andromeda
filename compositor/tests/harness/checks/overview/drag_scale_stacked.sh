#!/usr/bin/env bash
# harness: outputs=2
# Spaces overview on two stacked outputs: a card dragged from the top output onto the bottom output's thumb row shrinks
# to that row's thumb scale, as it does over its own output's row.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272 KEY_ESC=1
readonly W=1280 H=720
readonly RED='r > 0.6 && g < 0.3 && b < 0.3'
# The middle of the Spaces thumb row, output-local (style.cpp: the row sits in the top margin).
readonly ROW_Y=105
IMAGE="$ANDROMEDA_RUNTIME_DIR/stacked.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[overview]
style = "spaces"
background_blur = false
[output."HEADLESS-1"]
position = [0, 0]
[output."HEADLESS-2"]
position = [0, 720]
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null

count() {
  grim -o "$1" "$IMAGE"
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$RED"
}

"$ANDROMEDA" msg focus-output:HEADLESS-1 > /dev/null 2>&1 || true
"$ANDROMEDA_POINTER_CLIENT" "$W" $((2 * H)) move 640 360
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 600 400 > /dev/null 2>&1 &
for _ in $(seq 80); do
  [[ $("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "red") | .output') == HEADLESS-1 ]] && break
  sleep 0.025
done
[[ $("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "red") | .output') == HEADLESS-1 ]] ||
  { echo "red did not map on HEADLESS-1: $("$ANDROMEDA" windows --json)"; exit 1; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null

grim -o HEADLESS-1 "$IMAGE"
read -r rx ry rw rh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$RED")
((rw > 0)) || { echo "no red card on HEADLESS-1"; exit 1; }
cx=$((rx + rw / 2))
cy=$((ry + rh / 2))
pointer_hold "$W" $((2 * H)) move "$cx" "$cy" press "$BTN_LEFT" move "$cx" $((cy + 4)) move "$cx" $((cy + 9)) \
  move "$cx" $((cy + 20)) -- move 400 $((H + ROW_Y + 6)) move 400 $((H + ROW_Y)) mark over hold tap "$KEY_ESC" \
  release "$BTN_LEFT"
"$ANDROMEDA" settle > /dev/null
lifted=$(count HEADLESS-1)
pointer_step over
"$ANDROMEDA" settle > /dev/null
over=$(count HEADLESS-2)
pointer_release
"$ANDROMEDA" settle > /dev/null
# The thumb scale is a fraction of the main area's: the card covers well under half its lifted area there.
((over > 0 && over * 3 < lifted)) ||
  { echo "over the lower output's thumb row the card kept its size: red px lifted $lifted, over the row $over"; exit 1; }
echo "a card held over the other output's thumb row shrinks to its thumbs: red px $lifted -> $over"
