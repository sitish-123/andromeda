#!/usr/bin/env bash
# Each event must introduce a shader-only color during its transition and
# remove that color afterwards. Geometry settling alone cannot satisfy this.
set -euo pipefail
cat > "$ANDROMEDA_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
readonly BASE="$ANDROMEDA_RUNTIME_DIR/shader-events-base.toml"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/shader-events.png"
cp "$ANDROMEDA_CONFIG" "$BASE"

configure() {
  cat "$BASE" > "$ANDROMEDA_CONFIG"
  cat >> "$ANDROMEDA_CONFIG" <<EOF

[animation]
duration_ms = 300
curve = "linear"
[animation.windows_in]
enabled = false
[effects.preset.${1}_fixture_1]
kind = "animation"
shader = "fixture-1.glsl"
[animation.$1]
enabled = true
effect = "${1}_fixture_1"
EOF
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" clock-advance 1000
}

red_pixels() {
  grim "$IMAGE"
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1'
}
assert_no_red() {
  local count
  count=$(red_pixels)
  if (( count > 40 )); then
    echo "$1: stale shader color before transition or after completion ($count pixels)"
    exit 1
  fi
}
assert_transition() {
  local count
  "$ANDROMEDA" clock-advance 100
  count=$(red_pixels)
  if (( count < 120 )); then
    echo "$1: missing shader-only intermediate color ($count pixels)"
    exit 1
  fi
  "$ANDROMEDA" clock-advance 1000
  assert_no_red "$1"
  echo "$1 shader transition verified"
}

# Animation time only moves by clock-advance: each 300 ms transition is sampled 100 ms after its trigger, and
# advancing 1000 ms finishes it.
"$ANDROMEDA" clock-freeze
# Layer mapping is client-driven, so the transition starts only once the compositor has seen it.
wait_for_mapped_layers() {
  for _ in $(seq 100); do
    [[ $("$ANDROMEDA" layers --json | jq '[.[] | select(.mapped)] | length') == "$1" ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $1 mapped layer surfaces"
  exit 1
}

configure windows_move
"$ANDROMEDA_UNMAP_CLIENT" shader-events-a 700 700 > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
"$ANDROMEDA_UNMAP_CLIENT" shader-events-b 700 700 > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq length) == 2 ]] && break
  sleep 0.05
done
"$ANDROMEDA" clock-advance 1000
first=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "shader-events-a") | .id')
second=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "shader-events-b") | .id')
assert_no_red move
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
assert_transition resize

configure border
"$ANDROMEDA" msg "window-focus:$second" > /dev/null
"$ANDROMEDA" clock-advance 1000
assert_no_red border
"$ANDROMEDA" msg "window-focus:$first" > /dev/null
assert_transition border

configure dim_unfocused
assert_no_red dim_unfocused
"$ANDROMEDA" msg "window-focus:$second" > /dev/null
assert_transition dim_unfocused

configure workspaces
assert_no_red workspaces
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
assert_transition workspaces
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 1000

configure overview
assert_no_red overview
"$ANDROMEDA" msg overview-open > /dev/null
assert_transition overview
"$ANDROMEDA" msg overview-close > /dev/null
assert_transition overview-close

configure scratchpad
"$ANDROMEDA" msg window-move-to-scratchpad > /dev/null
"$ANDROMEDA" clock-advance 1000
assert_no_red scratchpad
"$ANDROMEDA" msg scratchpad-toggle > /dev/null
assert_transition scratchpad-show
"$ANDROMEDA" msg scratchpad-toggle > /dev/null
assert_transition scratchpad-hide

configure layers
assert_no_red layers
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 40 > "$ANDROMEDA_RUNTIME_DIR/layer.log" 2>&1 &
layer_pid=$!
wait_for_mapped_layers 1
assert_transition layer-open
kill -TERM "$layer_pid"
wait_for_mapped_layers 0
assert_transition layer-close
