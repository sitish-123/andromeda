#!/usr/bin/env bash
# harness: outputs=2
# Task View overview on a tall output beside a short one: a card grabbed level with the short output's thumb row and
# carried onto the short output far above that row stays near its lifted size. It reaches the thumb scale only in the
# row.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272 KEY_ESC=1
readonly RED='r > 0.6 && g < 0.3 && b < 0.3'
readonly LAYOUT_W=3840 LAYOUT_H=1440
IMAGE="$ANDROMEDA_RUNTIME_DIR/mixed.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[overview]
style = "taskview"
background_blur = false
[output."HEADLESS-1"]
mode = "2560x1440"
position = [0, 0]
[output."HEADLESS-2"]
mode = "1280x720"
position = [2560, 0]
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null

count() {
  grim -o "$1" "$IMAGE"
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$RED"
}

"$ANDROMEDA_POINTER_CLIENT" "$LAYOUT_W" "$LAYOUT_H" move 1280 720
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 1200 900 > /dev/null 2>&1 &
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
# The short output's Task View row lies in its bottom margin, about y 620-700.
readonly GRAB_Y=660
((rw > 0 && ry < GRAB_Y - 20 && ry + rh > GRAB_Y + 20)) ||
  { echo "the red card ${rw}x${rh}+${rx}+${ry} does not span y $GRAB_Y"; exit 1; }
cx=$((rx + rw / 2))
pointer_hold "$LAYOUT_W" "$LAYOUT_H" move "$cx" "$GRAB_Y" press "$BTN_LEFT" move "$cx" $((GRAB_Y - 4)) \
  move "$cx" $((GRAB_Y - 9)) move "$cx" $((GRAB_Y - 20)) -- move 3200 300 move 3200 200 mark far hold \
  tap "$KEY_ESC" release "$BTN_LEFT"
"$ANDROMEDA" settle > /dev/null
lifted=$(count HEADLESS-1)
pointer_step far
"$ANDROMEDA" settle > /dev/null
far=$(count HEADLESS-2)
pointer_release
"$ANDROMEDA" settle > /dev/null
((far * 3 > lifted)) ||
  { echo "500 px above the short output's row the card already shrank to thumb size: red px lifted $lifted, there $far"; exit 1; }
echo "far above the other output's row the card keeps most of its size: red px $lifted -> $far"
