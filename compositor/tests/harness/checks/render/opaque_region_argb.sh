#!/usr/bin/env bash
# A window whose ARGB8888 buffer is half transparent premultiplied white and whose opaque region still covers the whole
# surface (as Zen Browser's sidebar does) blends over the backdrop: white over white stays white, not mid grey.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/opaque-lie.log"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/opaque-lie.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false

[colors]
backdrop = "#FFFFFFFF"

[appearance]
border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false
CONF
"$ANDROMEDA" msg config-reload > /dev/null

"$CLIENT" opaque-lie > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the opaque-lie window"
"$ANDROMEDA" settle
read -r x y w h < <("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "opaque-lie") | "\(.x) \(.y) \(.w) \(.h)"')
grim "$SHOT"
region="$((w - 8))x$((h - 8))+$((x + 4))+$((y + 4))"
light=$("$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'r > 0.95 && g > 0.95 && b > 0.95' "$region")
total=$(((w - 8) * (h - 8)))
if ((light != total)); then
  echo "the translucent window was not blended over the backdrop: $light of $total pixels are light in $region"
  exit 1
fi
echo "opaque region over an ARGB buffer blended: $light of $total pixels light"
