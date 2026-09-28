#!/usr/bin/env bash
# wp_alpha_modifier_v1 is offered, and a white window's multiplier blends it over a black backdrop: 1 is white, 0.5
# is mid grey, and 0 shows the backdrop.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/alpha.log"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/alpha-control"

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

mkfifo "$FIFO"
exec {control}<> "$FIFO"
"$CLIENT" alpha <&"$control" > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the alpha window"
"$UMBRIEL" settle
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "alpha") | "\(.x) \(.y) \(.w) \(.h)"')
center="20x20+$((x + w / 2 - 10))+$((y + h / 2 - 10))"

# Mean red of the window's centre, 0 to 255, at multiplier $1.
red_at() {
  local count
  count=$(events "$LOG" "alpha $1")
  echo "alpha $1" >&"$control"
  await_events "$LOG" "alpha $1" $((count + 1)) "multiplier $1" > /dev/null
  "$UMBRIEL" settle
  grim "$UMBRIEL_RUNTIME_DIR/alpha-$1.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/alpha-$1.png" mean "$center" | cut -d' ' -f1
}

opaque=$(red_at 1)
half=$(red_at 0.5)
clear=$(red_at 0)
if ((opaque < 250 || half < 110 || half > 145 || clear > 5)); then
  echo "the multiplier did not blend the window: red $opaque at 1, $half at 0.5, $clear at 0"
  exit 1
fi
echo "alpha multiplier blended white over black: red $opaque at 1, $half at 0.5, $clear at 0"
