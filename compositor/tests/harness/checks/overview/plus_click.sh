#!/usr/bin/env bash
# Clicking the Spaces + target adds a desktop and goes there, with the overview open.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
IMAGE="$ANDROMEDA_RUNTIME_DIR/plus.png"
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
count() { "$ANDROMEDA" workspaces --json | jq 'length'; }

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" plus-red 600 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq length) -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
first=$(active)
before=$(count)
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

# The + glyph is the rightmost light mark in the thumb row.
grim -o HEADLESS-1 "$IMAGE"
read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.45 && g > 0.45 && b > 0.45 && r < 0.9' "$BAND")
((w > 0)) || { echo "no + mark in the thumb row"; exit 1; }
"$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move "$((x + w - 3))" "$((y + h / 2))" click "$BTN_LEFT"
for _ in $(seq 60); do
  [[ $(active) != "$first" ]] && break
  sleep 0.05
done
[[ $(active) != "$first" ]] || { echo "clicking + left desktop $first active ($(count) desktops, $before before)"; exit 1; }
echo "clicking + went to a new desktop ($before desktops before, $(count) after)"
