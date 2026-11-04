#!/usr/bin/env bash
# A background child surface that changes under a blurred window re-renders only the cached blur it touches: the child's
# box grown by the blur reach, inside the blurred window's grown box. It never re-renders the output, and a change
# outside the reach re-renders nothing. The output scale is 0.5, so the reach is twice as large in layout pixels as
# the kernel is in device pixels. Background layers that fade out and close must leave no tracked node behind.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/blur-rect-layer.log"
readonly BAR_W=1200
readonly BAR_H=400
readonly CHILD=200
readonly REACH=32 # 2^(passes + 1) * radius, in device pixels
readonly SCALE_DIV=2 # output scale 0.5
readonly OUTPUT_AREA=$((1280 * 720))

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 4
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[animation.layers]
enabled = false
duration_ms = 150
curve = "linear"
[output.HEADLESS-1]
scale = 0.5
[colors]
backdrop = "#00FF00FF"
[[window_rule]]
match.title = "^optimized-blur-rect-bar$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
blur = true
blur_optimized = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

env TRANSLUCENT_CONTENT=1 "$ANDROMEDA_SUBSURFACE_CLIENT" optimized-blur-rect-bar "$BAR_W" "$BAR_H" \
  > "$ANDROMEDA_RUNTIME_DIR/blur-rect-bar.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/blur-rect-bar.log" mapped 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label callbacks 1" 1
  "$ANDROMEDA" settle > /dev/null
}

blur_stat() { "$ANDROMEDA" blur-cache-pixels --json | jq -er "(.ok // .).$1.\"HEADLESS-1\""; }
pixels() { blur_stat pixels; }

# Park the child far from the bar, then change it: nothing within reach of the bar changed.
layer_command child-color far-map FFFF0000
layer_command child-move far-move 2200 1200
layer_command child-color far-green FF00FF00
base=$(pixels)
layer_command child-color far-blue FF0000FF
far=$(pixels)
((far == base)) || { echo "a change outside the blur reach re-rendered $((far - base)) cache pixels"; exit 1; }

# Put the child inside the bar, with its grown box inside the bar's grown box, then change it.
layer_command child-move near-move 400 200
base=$(pixels)
layer_command child-color near-green FF00FF00
near=$(pixels)
delta=$((near - base))
# Device pixels of the child box grown by the reach on each side.
expected=$(((CHILD / SCALE_DIV + 2 * REACH) * (CHILD / SCALE_DIV + 2 * REACH)))
limit=$(((BAR_W / SCALE_DIV + 2 * REACH) * (BAR_H / SCALE_DIV + 2 * REACH)))
((delta > 0)) || { echo "a child under the blurred window re-rendered no cache pixels"; exit 1; }
((delta <= limit)) || { echo "re-rendered $delta cache pixels, more than the bar's grown box $limit"; exit 1; }
((delta * 4 < OUTPUT_AREA)) || { echo "re-rendered $delta of $OUTPUT_AREA output pixels"; exit 1; }
((delta >= expected * 98 / 100 && delta <= expected * 110 / 100)) || {
  echo "re-rendered $delta cache pixels, expected the grown child box of $expected"
  exit 1
}

# Closed background layers must not leave tracked nodes behind.
sed -i '/^\[animation.layers\]$/,/^\[/s/^enabled = false$/enabled = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
nodes_before=$(blur_stat nodes)
for i in 1 2 3 4 5; do
  "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 > "$ANDROMEDA_RUNTIME_DIR/blur-rect-wallpaper-$i.log" 2>&1 &
  wallpaper_pid=$!
  await_lines "$ANDROMEDA_RUNTIME_DIR/blur-rect-wallpaper-$i.log" ready 1
  kill "$wallpaper_pid"
  wait "$wallpaper_pid" || true
  "$ANDROMEDA" settle > /dev/null
done
nodes_after=$(blur_stat nodes)
((nodes_after == nodes_before)) || { echo "closing 5 background layers left $((nodes_after - nodes_before)) tracked nodes"; exit 1; }

echo "optimized blur re-rendered $delta cache pixels (grown child box $expected) for a child change under the bar, none outside its reach, and tracked no node past layer close"
