#!/usr/bin/env bash
# wp_single_pixel_buffer_manager_v1 is offered, and a window whose content is one red single-pixel buffer scaled by
# wp_viewporter shows solid red across its box.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/single-pixel.log"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/single-pixel.png"

cat >> "$UMBRIEL_CONFIG" <<'CONF'

[animation]
enabled = false

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false
CONF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" single-pixel > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the single-pixel window"
"$UMBRIEL" settle
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "single-pixel") | "\(.x) \(.y) \(.w) \(.h)"')
grim "$SHOT"
region="$((w - 8))x$((h - 8))+$((x + 4))+$((y + 4))"
red=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" count 'r > 0.95 && g < 0.05 && b < 0.05' "$region")
total=$(((w - 8) * (h - 8)))
if ((red != total)); then
  echo "the single-pixel window is not solid red: $red of $total pixels in $region"
  exit 1
fi
echo "single-pixel buffer filled ${w}x$h with red"
