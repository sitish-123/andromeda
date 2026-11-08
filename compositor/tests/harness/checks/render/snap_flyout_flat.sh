#!/usr/bin/env bash
# The Snap Layouts flyout draws flat zones: the accent of the lit zone stays inside its rectangle and does not glow into
# the panel around it. The open animation matters: its frames once left the zone in the pixels the glass blur sampled.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'
[animation]
enabled = true
[layout]
mode = "floating"
[appearance.title_bar]
mode = "forced"
[colors.title_bar]
maximize = "#808080FF"
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^flyout-flat$"
default_floating = true
default_floating_size_px = { width = 900, height = 560 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$ANDROMEDA_UNMAP_CLIENT" flyout-flat 900 560 > "$ANDROMEDA_RUNTIME_DIR/window.log" 2>&1 &
title_bar_wait_field flyout-flat floating true
"$ANDROMEDA" msg window-snap-layouts > /dev/null
"$ANDROMEDA" settle > /dev/null
image=$ANDROMEDA_RUNTIME_DIR/flyout.png
grim -o HEADLESS-1 "$image"

# The lit zone (the keyboard path lights the first zone) in accent: strong blue, low red and green.
read -r zx zy zw zh < <("$ANDROMEDA_PIXEL_PROBE" "$image" bbox 'b > 0.8 && r < 0.45 && g < 0.4')
((zw > 20 && zh > 30)) || { echo "no lit zone: $zx,$zy ${zw}x$zh"; exit 1; }
blue() {
  local r g b
  read -r r g b < <("$ANDROMEDA_PIXEL_PROBE" "$image" pixel "$1" "$2")
  echo $((b - r))
}
row=$((zy + zh / 2))
# 2 px into the 4 px gap right of the lit zone, against the 8 px layout gap one mini further right.
near=$(blue "$((zx + zw + 2))" "$row")
far=$(blue "$((zx + 2 * zw + 4 + 4))" "$row")
((near - far <= 6)) || { echo "accent bleeds into the panel: blue-red $near beside the zone, $far away from it"; exit 1; }
echo "the lit zone stays flat: blue-red $near beside it, $far away"
