#!/usr/bin/env bash
# An animated overview open re-blurs the cached wallpaper once, not on every frame: a cache re-render that reaches the
# output edge clears its pending area.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[output.HEADLESS-1]
mode = "1280x720"
scale = 1.5

[appearance.blur]
enabled = true
optimized = true

[overview]
style = "taskview"
background_blur = true

[animation.overview]
enabled = true
duration_ms = 400
curve = "linear"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 > "$ANDROMEDA_RUNTIME_DIR/wall.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/wall.log" ready 1
FILL_COLOR=0xFF3D7BFD "$ANDROMEDA_UNMAP_CLIENT" blur-cache 600 400 > "$ANDROMEDA_RUNTIME_DIR/w.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/w.log" mapped 1
"$ANDROMEDA" settle

pixels() { "$ANDROMEDA" blur-cache-pixels --json | jq -er '(.ok // .).pixels."HEADLESS-1"'; }
frames() { "$ANDROMEDA" output-commits --json | jq -er '(.ok // .)."HEADLESS-1"'; }
before=$(pixels)
commits=$(frames)
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
drawn=$(($(frames) - commits))
rerendered=$(($(pixels) - before))
output=$((1280 * 720))
((drawn > 4)) || { echo "the overview open drew only $drawn frames; the check needs an animated open"; exit 1; }
if ((rerendered > 2 * output)); then
  echo "opening the overview re-blurred $rerendered cached pixels over $drawn frames; one output is $output"
  exit 1
fi
echo "the open re-blurred the wallpaper cache once ($rerendered px over $drawn frames)"
