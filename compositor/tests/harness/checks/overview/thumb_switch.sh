#!/usr/bin/env bash
# Clicking another desktop's Spaces thumb shows that desktop and keeps the overview open.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
IMAGE="$ANDROMEDA_RUNTIME_DIR/thumb-switch.png"
EVENTS="$ANDROMEDA_RUNTIME_DIR/thumb-switch-events.log"
# The thumb row: 56 px from the top, a thumb about 99 px high at this size.
readonly BAND="${OUTPUT_W}x99+0+56"

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[overview]
style = "spaces"
background_blur = false

[animation]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

active() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .index'; }
wait_windows() {
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq length) -eq $1 ]] && return 0
    sleep 0.05
  done
  echo "expected $1 windows"
  exit 1
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" thumb-red 600 400 > /dev/null 2>&1 &
wait_windows 1
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" thumb-blue 600 400 > /dev/null 2>&1 &
wait_windows 2
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle
first=$(active)

"$ANDROMEDA" subscribe overview > "$EVENTS" &
for _ in $(seq 40); do
  [[ -s $EVENTS ]] && break
  sleep 0.05
done
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

grim -o HEADLESS-1 "$IMAGE"
read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.6 && r < 0.3 && g < 0.3' "$BAND")
((w > 0)) || { echo "no blue window in the thumb row"; exit 1; }
"$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move "$((x + w / 2))" "$((y + h / 2))" click "$BTN_LEFT"
for _ in $(seq 60); do
  [[ $(active) != "$first" ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
[[ $(active) != "$first" ]] || { echo "clicking the desktop 2 thumb left desktop $first active"; exit 1; }
open=$(jq -s '[.[] | select(.event == "overview")] | last | .data.open' "$EVENTS")
[[ $open == true ]] || { echo "clicking a thumb closed the overview (last overview event open=$open)"; exit 1; }
echo "clicking the desktop 2 thumb switched to desktop $(active) with the overview open"
