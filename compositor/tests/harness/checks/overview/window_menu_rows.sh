#!/usr/bin/env bash
# The window menu renames the card's desktop (Rename Desktop, fourth row from the bottom), and Ctrl+W closes the
# selected window (Caps Lock on too), with the overview open (DESIGN.md "Overview v3" Completion).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_RIGHT=273
readonly K_UP=103 K_ENTER=28 K_TAB=15 K_W=17 K_A=30 K_B=48
IMAGE="$ANDROMEDA_RUNTIME_DIR/rows.png"

pointer() {
  "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"
  "$ANDROMEDA" settle
}
label() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .label'; }
count() { "$ANDROMEDA" windows --json | jq 'length'; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "floating"

[overview]
style = "spaces"
background_blur = false

[animation]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" rows-blue 500 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $(count) -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

grim -o HEADLESS-1 "$IMAGE"
read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
((w > 0)) || { echo "no blue card on screen"; exit 1; }
# From the bottom: Close All, Close, Arrangement, Rename Desktop.
pointer move "$((x + w / 2))" "$((y + h / 2))" click "$BTN_RIGHT" tap $K_UP tap $K_UP tap $K_UP tap $K_UP tap $K_ENTER \
  tap $K_A tap $K_B tap $K_ENTER
[[ $(label) == ab ]] || { echo "Rename Desktop, a, b, Enter labelled the desktop '$(label)'"; exit 1; }

# Caps Lock on: Ctrl+W follows the key, not the Caps level.
pointer tap $K_TAB lock caps mod ctrl tap $K_W mod none lock none
for _ in $(seq 60); do
  [[ $(count) -eq 0 ]] && break
  sleep 0.05
done
[[ $(count) -eq 0 ]] || { echo "Ctrl+W left the selected window open"; exit 1; }
echo "the window menu renames the card's desktop, and Ctrl+W closes the selected window"
