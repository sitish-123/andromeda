#!/usr/bin/env bash
# Spaces overview on a 5:4 output, whose thumb row is taller than its gap to the main area: a card grabbed just below
# the row shrinks smoothly into it. Sampled every 6 px across the row's bottom edge, no step halves the card's area:
# the scale reaches the thumb scale at the edge instead of jumping there one pixel inside.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272 KEY_ESC=1 W=1280 H=1024
readonly RED='r > 0.6 && g < 0.3 && b < 0.3'
IMAGE="$ANDROMEDA_RUNTIME_DIR/edge.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[overview]
style = "spaces"
background_blur = false
[output."HEADLESS-1"]
mode = "1280x1024"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 1280 1024 > /dev/null 2>&1 &
for _ in $(seq 80); do
  [[ -n $("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "red") | .id') ]] && break
  sleep 0.025
done
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
read -r rx ry rw rh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$RED")
((rw > 0)) || { echo "no red card"; exit 1; }
cx=$((rx + rw / 2))
grab=$((ry + 6))

# Walk y 250 -> 130 across the row's bottom edge.
steps=()
for y in $(seq 250 -6 130); do
  steps+=(move "$cx" "$y" mark "y$y" hold)
done
pointer_hold "$W" "$H" move "$cx" "$grab" press "$BTN_LEFT" move "$cx" $((grab + 4)) move "$cx" $((grab + 9)) \
  -- "${steps[@]}" tap "$KEY_ESC" release "$BTN_LEFT"
areas=()
for y in $(seq 250 -6 130); do
  pointer_step "y$y"
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$IMAGE"
  areas+=("$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$RED")")
done
pointer_release
"$ANDROMEDA" settle > /dev/null
((areas[0] > 2 * areas[${#areas[@]} - 1])) ||
  { echo "grabbed at y $grab, the card did not shrink into the row: red px ${areas[*]}"; exit 1; }
for i in $(seq 1 $((${#areas[@]} - 1))); do
  ((2 * areas[i] >= areas[i - 1])) ||
    { echo "grabbed at y $grab, a 6 px step at y $((250 - 6 * i)) jumped the card's size: red px ${areas[*]}"; exit 1; }
done
echo "grabbed at y $grab, the card shrinks into the row without a jump: red px ${areas[*]}"
