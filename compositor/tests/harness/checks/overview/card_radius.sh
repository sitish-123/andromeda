#!/usr/bin/env bash
# A window card rounds its corners like the live window, scaled by the card scale (DESIGN.md "Overview v3" Card
# parity), in every arrangement and overview style, with a border and without one: border_width = 0 still rounds.
# The radius is read from the pixels a corner cuts: a quarter-circle cut of radius r misses r^2 (1 - pi/4).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
IMAGE="$ANDROMEDA_RUNTIME_DIR/radius.png"

configure() {
  cat > "$ANDROMEDA_CONFIG" <<EOF
[general]
xwayland = false
autostart = []

[layout]
mode = "$1"

[appearance]
corner_radius = 40
border_width = $3

[overview]
style = "$2"
background_blur = false

[animation]
enabled = false
EOF
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" settle
}
configure floating spaces 0

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" radius-card 800 500 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 5 5
"$ANDROMEDA" settle

red='r > 0.9 && g < 0.1 && b < 0.1'
# Prints "<content width> <radius>" from the bottom-left corner of the red content box.
measure() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$red")
  ((w > 40 && h > 40)) || { echo "no red content on screen ($x $y $w $h)" >&2; return 1; }
  local filled
  filled=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$red" "40x40+$x+$((y + h - 40))")
  awk -v w="$w" -v missing="$((1600 - filled))" 'BEGIN { printf "%d %.2f\n", w, sqrt(missing / (1 - 3.14159265 / 4)) }'
}

# Every arrangement and style without a border (the case that went square), one pair with a border.
combos=()
for mode in floating scrolling dwindle master; do
  for style in spaces taskview strip grid; do combos+=("0 $mode $style"); done
done
combos+=("2 scrolling spaces")
failed=0
for combo in "${combos[@]}"; do
  read -r border mode style <<< "$combo"
  configure "$mode" "$style" "$border"
  read -r live_w live_r < <(measure)
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle
  read -r card_w card_r < <(measure)
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" settle
  awk -v m="border $border $mode/$style" -v lw="$live_w" -v lr="$live_r" -v cw="$card_w" -v cr="$card_r" 'BEGIN {
    want = lr * cw / lw
    if (cr < want - 1.5 || cr > want + 1.5) {
      printf "%s: card radius %.1f px, want %.1f (live %.1f x scale %.3f)\n", m, cr, want, lr, cw / lw; exit 1
    }
  }' || failed=1
done
((failed == 0)) || exit 1
echo "in every arrangement and overview style the card corner radius is the live radius times the card scale"
