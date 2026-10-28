#!/usr/bin/env bash
# harness: outputs=1
# Border details and overlays follow overview zoom, including animation, fractional output scale, and snapshots.
set -euo pipefail
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/overview-effect-scale.png"
cat > "$ANDROMEDA_RUNTIME_DIR/ring.glsl" <<'GLSL'
vec4 border(vec2 uv) {
  return andromeda_border_distance(uv) < 20.0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0);
}
GLSL
cat > "$ANDROMEDA_RUNTIME_DIR/overlay.glsl" <<'GLSL'
vec4 window(vec2 uv) {
  float edge = min(uv.x, 1.0 - uv.x) * andromeda_size.x;
  return edge < 40.0 ? vec4(0.0, 1.0, 0.0, 1.0) : andromeda_sample(uv);
}
GLSL
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 40
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[colors]
backdrop = "#000000FF"
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
[effects]
border = "ring"
in_capture = true
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
animated = false
padding = 40
overlay = "overlay"
[effects.preset.overlay]
kind = "window"
shader = "overlay.glsl"
[[window_rule]]
match.title = "^overview-effect-scale$"
default_floating = true
default_position = { x = 80, y = 80, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" overview-effect-scale 400 300 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$ANDROMEDA" windows --json | jq -c '.[] | select(.title == "overview-effect-scale")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$ANDROMEDA" settle > /dev/null

measure() {
  local label=$1 zoom=$2 output_scale=${3:-1} x y w h image_w image_h green red expected_green expected_red
  grim "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 || g > 0.9')
  if (( w == 0 || h == 0 )); then
    echo "$label: no client pixels"
    exit 1
  fi
  IFS=x read -r image_w image_h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" size)
  local row="${image_w}x1+0+$((y + h / 2))"
  green=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "$row")
  red=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1' "$row")
  expected_green=$(awk -v z="$zoom" -v s="$output_scale" 'BEGIN {printf "%.0f", 80*z*s}')
  expected_red=$(awk -v z="$zoom" -v s="$output_scale" 'BEGIN {printf "%.0f", 40*z*s}')
  if (( green < expected_green - 2 || green > expected_green + 2 )); then
    echo "$label: inner overlay is $green pixels wide, expected $expected_green"
    exit 1
  fi
  if (( red < expected_red - 3 || red > expected_red + 3 )); then
    echo "$label: border details are $red pixels wide, expected $expected_red"
    exit 1
  fi
  echo "$label: overlay=$green border=$red"
}

measure desktop 1
for zoom in 0.5 0.1; do
  sed -i "s/^zoom = .*/zoom = $zoom/" "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle > /dev/null
  measure "overview $zoom" "$zoom"
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" settle > /dev/null
  measure "desktop after $zoom" 1
done

sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$ANDROMEDA_CONFIG"
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation.overview]
enabled = true
duration_ms = 1000
curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" clock-advance 500
measure "opening halfway" 0.55
"$ANDROMEDA" clock-advance 500
"$ANDROMEDA" settle > /dev/null
measure "opening finished" 0.1
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" clock-advance 500
measure "closing halfway" 0.55
"$ANDROMEDA" clock-advance 500
"$ANDROMEDA" settle > /dev/null
measure "closing finished" 1
"$ANDROMEDA" clock-resume

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.25
transform = "90"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
measure "rotated desktop" 1 1.25
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
measure "rotated overview 0.1" 0.1 1.25

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" clock-freeze
id=$(jq -r '.id' <<< "$window")
"$ANDROMEDA" msg "window-close:$id" > /dev/null
for _ in $(seq 100); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 0 ]] && break
  sleep 0.02
done
[[ $("$ANDROMEDA" windows --json | jq 'length') -eq 0 ]]
"$ANDROMEDA" clock-advance 1
measure "overview close snapshot" 0.1 1.25
"$ANDROMEDA" clock-advance 5000
"$ANDROMEDA" clock-resume
