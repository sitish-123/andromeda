#!/usr/bin/env bash
# The wallpaper blur cache keeps only wallpaper. A glass surface (a panel, a blurred window) over an opaque window makes
# the cache re-blur under it; that frame draws no wallpaper there, so storing those pixels keeps a blurred copy of the
# window, which the overview background later shows as a window-shaped ghost.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[layout]
mode = "floating"

[appearance.blur]
enabled = true
optimized = true
radius = 8

[colors.overview]
background_tint = "#00000000"
workspace_background = "#00000000"

[overview]
style = "taskview"
background_blur = true
workspace_wallpaper = true

[animation.overview]
enabled = false

[[window_rule]]
match.title = "^glass$"
blur = true
blur_optimized = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 > "$ANDROMEDA_RUNTIME_DIR/wall.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/wall.log" ready 1
"$ANDROMEDA" settle
# Mean red of a 40 px patch at x,y.
red() { magick "$1" -crop "40x40+$2+$3" -format '%[fx:round(255*mean.r)]' info:; }

OPAQUE=1 FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" solid 600 400 > "$ANDROMEDA_RUNTIME_DIR/solid.log" 2>&1 &
solid=$!
await_lines "$ANDROMEDA_RUNTIME_DIR/solid.log" mapped 1
FILL_COLOR=0x40FFFFFF "$ANDROMEDA_UNMAP_CLIENT" glass 500 300 > "$ANDROMEDA_RUNTIME_DIR/glass.log" 2>&1 &
glass=$!
await_lines "$ANDROMEDA_RUNTIME_DIR/glass.log" mapped 1
"$ANDROMEDA" settle
# A blur parameter change re-blurs the cache where the glass window samples it, over the red window.
sed -i 's/^radius = 8$/radius = 9/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle
kill "$glass" "$solid"
wait "$glass" "$solid" 2> /dev/null || true
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
grim "$ANDROMEDA_RUNTIME_DIR/open.png"
cp "$ANDROMEDA_RUNTIME_DIR/open.png" /tmp/blur-cache-occluded-open.png
# Centre of the old windows against a corner they never covered.
under=$(red "$ANDROMEDA_RUNTIME_DIR/open.png" 620 340)
clear=$(red "$ANDROMEDA_RUNTIME_DIR/open.png" 60 600)
if (( under > clear + 20 )); then
  echo "overview background red $under where the windows were, $clear in the corner: the cache kept the window"
  exit 1
fi
