#!/usr/bin/env bash
# Output-relative reveals survive every transform, fractional scale and nonzero layout origins.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly BASE="$ANDROMEDA_RUNTIME_DIR/base.toml"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/geometry.png"
workspace_reveal_config
cp "$ANDROMEDA_CONFIG" "$BASE"
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" geometry-a 1280 720 > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/a.log" mapped 1
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle
FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" geometry-b 1280 720 > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/b.log" mapped 1
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
for transform in normal 90 180 270 flipped flipped-90 flipped-180 flipped-270; do
  cp "$BASE" "$ANDROMEDA_CONFIG"
  cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[output.HEADLESS-1]
position = [137, -53]
scale = 1.25
transform = "$transform"
workspace_axis = "horizontal"
CONFIG
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" clock-freeze
  "$ANDROMEDA" msg workspace-switch:1 > /dev/null
  "$ANDROMEDA" clock-advance 1000
  "$ANDROMEDA" clock-resume
  "$ANDROMEDA" settle
  "$ANDROMEDA" clock-freeze
  "$ANDROMEDA" msg workspace-switch:2 > /dev/null
  "$ANDROMEDA" clock-advance 500
  grim -s 1 -o HEADLESS-1 "$IMAGE"
  IFS=x read -r width height < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" size)
  left=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((width / 4))" "$((height / 2))")
  right=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((width * 3 / 4))" "$((height / 2))")
  [[ $left == '0 0 255' && $right == '255 0 0' ]] || {
    echo "$transform: logical reveal orientation failed, $width x $height: left=$left right=$right"; exit 1;
  }
  "$ANDROMEDA" clock-advance 1000
  "$ANDROMEDA" clock-resume
  "$ANDROMEDA" settle
done
# Changing geometry during a held reveal must leave the native active destination intact.
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 500
sed -i 's/^scale = 1.25$/scale = 1.5/; s/^transform = .*/transform = "normal"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
grim -s 1 -o HEADLESS-1 "$IMAGE"
IFS=x read -r width height < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" size)
[[ $("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((width / 2))" "$((height / 2))") == '255 0 0' ]]
echo 'eight transforms, fractional scale, nonzero origin and mid-reveal geometry change verified'
