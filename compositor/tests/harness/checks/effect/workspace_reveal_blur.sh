#!/usr/bin/env bash
# Real transparent clients keep their native blur backdrop during a captured reveal.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/reveal-blur.png"
workspace_reveal_config
cat > "$ANDROMEDA_RUNTIME_DIR/blue.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0); }
GLSL
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[colors]
backdrop = "#00FF00FF"
[effects]
in_capture = false
[effects.preset.blue]
kind = "window"
shader = "blue.glsl"
[appearance.blur]
enabled = true
optimized = false
passes = 3
radius = 12
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[[window_rule]]
match.title = "^blur-"
default_floating = true
default_position = { x = 140, y = 60, anchor = "top_left" }
blur = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 32 > "$ANDROMEDA_RUNTIME_DIR/panel.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/panel.log" ready 1
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" blur-a 1000 600 > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/a.log" mapped 1
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle
FILL_COLOR=0x00000000 "$ANDROMEDA_UNMAP_CLIENT" blur-b 1000 600 > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/b.log" mapped 1
"$ANDROMEDA" settle
b_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "blur-b") | .id')
check_pixel() {
  local got
  got=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$1" "$2")
  [[ $got == "$3" ]] || { echo "$4: wanted $3, got $got at $1,$2"; return 1; }
}
start_reveal() {
  "$ANDROMEDA" clock-freeze
  "$ANDROMEDA" msg workspace-switch:1 > /dev/null
  "$ANDROMEDA" clock-advance 1000
  "$ANDROMEDA" clock-resume
  "$ANDROMEDA" settle
  "$ANDROMEDA" clock-freeze
  "$ANDROMEDA" msg workspace-switch:2 > /dev/null
  "$ANDROMEDA" clock-advance 500
}
for optimized in false true; do
  sed -i "s/^optimized = .*/optimized = $optimized/" "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  start_reveal
  grim "$IMAGE"
  check_pixel 640 350 '0 255 0' "$optimized blur excludes outgoing near reveal edge"
  check_pixel 640 400 '255 0 0' 'outgoing remains visible'
  check_pixel 640 16 '32 32 32' 'shared panel retains stacking'
  "$ANDROMEDA" clock-advance 1000
  "$ANDROMEDA" clock-resume
  "$ANDROMEDA" settle
  grim "$IMAGE"
  check_pixel 640 350 '0 255 0' 'transparent destination endpoint'
done
# A lower window on the incoming workspace must remain in its non-optimized backdrop.
sed -i 's/^optimized = true$/optimized = false/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" blur-lower 1000 600 > "$ANDROMEDA_RUNTIME_DIR/lower.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/lower.log" mapped 1
"$ANDROMEDA" msg "window-focus:$b_id" > /dev/null
"$ANDROMEDA" settle
start_reveal
grim "$IMAGE"
check_pixel 640 350 '0 0 255' 'incoming blur retains own lower window'
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
# Restore optimized blur: its native source is the shared background even with a lower window.
sed -i 's/^optimized = false$/optimized = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
# The two-pass capture path must retain the reveal while excluding the window effect.
"$ANDROMEDA" msg "effect-window-set:blue/$b_id" > /dev/null
start_reveal
grim "$IMAGE"
check_pixel 640 350 '0 255 0' 'unfiltered capture retains reveal and native blur'
sed -i 's/^in_capture = false$/in_capture = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-advance 1
grim "$IMAGE"
check_pixel 640 350 '0 0 255' 'filtered capture includes window effect inside reveal'
check_pixel 640 400 '255 0 0' 'filtered capture preserves outgoing role'
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
# Compare a patterned blur edge with its native endpoint. The blue lower
# window meets the green backdrop here, well inside the incoming wipe half.
# A clipped backdrop must retain the blur's whole sampling margin.
sed -i 's/^optimized = true$/optimized = false/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg "effect-window-set:off/$b_id" > /dev/null
"$ANDROMEDA" settle
grim "$ANDROMEDA_RUNTIME_DIR/native-edge.png"
start_reveal
grim "$ANDROMEDA_RUNTIME_DIR/reveal-edge.png"
for name in native reveal; do
  magick "$ANDROMEDA_RUNTIME_DIR/$name-edge.png" -crop 120x100+130+50 +repage "$ANDROMEDA_RUNTIME_DIR/$name-crop.png"
done
error=$(magick "$ANDROMEDA_RUNTIME_DIR/native-crop.png" "$ANDROMEDA_RUNTIME_DIR/reveal-crop.png" \
  -compose difference -composite -format '%[fx:maxima*255]' info:)
awk -v error="$error" 'BEGIN { exit !(error <= 3) }' || {
  echo "isolated blur edge differs from native by $error channel levels"; exit 1;
}
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
echo 'real-client blur, optimized backdrop, shared panel and filtered/unfiltered reveal captures verified'
