#!/usr/bin/env bash
# Observe shader-specific intermediate colors, source sampling, completion,
# closing snapshots, and automatic reload of an imported GLSL file.
set -euo pipefail
cat > "$ANDROMEDA_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL

readonly CLIENT="$ANDROMEDA_UNMAP_CLIENT"
readonly SHADER="${ANDROMEDA_CONFIG%/*}/animation.glsl"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/shader.png"

cat > "$SHADER" <<'EOF'
vec4 animation(vec2 uv) {
    vec4 source = andromeda_sample(uv);
    return vec4(1.0, step(0.4, andromeda_progress), source.b > 0.01 ? 0.0 : 1.0, 1.0);
}
EOF
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = true
duration_ms = 1600
curve = "linear"
[effects.preset.windows_in_animation]
kind = "animation"
shader = "animation.glsl"
[animation.windows_in]
style = "fade"
effect = "windows_in_animation"
[effects.preset.windows_out_fixture_1]
kind = "animation"
shader = "fixture-1.glsl"
[animation.windows_out]
effect = "windows_out_fixture_1"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn() {
  "$CLIENT" "$1" 1200 700 > "$ANDROMEDA_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$ANDROMEDA" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  window_id=$(jq -r .id <<< "$window")
  sample_x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
  sample_y=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
}

sample() {
  grim "$IMAGE"
  read -r red green blue < <(magick "$IMAGE" -crop "8x8+${sample_x}+${sample_y}" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:)
}

# Animation time only moves by clock-advance: opening samples land at 120 ms and 770 ms (progress 0.075 and 0.48),
# the closing sample at 150 ms, and advancing 1600 ms finishes each timeline.
"$ANDROMEDA" clock-freeze
spawn shader-first
"$ANDROMEDA" clock-advance 120
sample
if ! (( red > 220 && green < 30 && blue < 30 )); then
  echo "opening shader did not sample the client and produce red: $red $green $blue"
  exit 1
fi
"$ANDROMEDA" clock-advance 650
sample
if ! (( red > 220 && green > 220 && blue < 30 )); then
  echo "shader progress did not transition red to yellow: $red $green $blue"
  exit 1
fi
"$ANDROMEDA" clock-advance 1600
"$ANDROMEDA" settle
sample
if ! (( blue > 80 && red < 200 )); then
  echo "opening shader did not release its target at completion: $red $green $blue"
  exit 1
fi

"$ANDROMEDA" msg "window-close:$window_id" > /dev/null
"$ANDROMEDA" clock-advance 150
sample
if ! (( green > 220 && red < 30 && blue < 30 )); then
  echo "closing snapshot did not run its shader: $red $green $blue"
  exit 1
fi
"$ANDROMEDA" clock-advance 1600
"$ANDROMEDA" settle

# Do not explicitly reload: the imported file must be a watcher dependency.
mark=$(wc -l < "$ANDROMEDA_LOG")
cat > "$SHADER" <<'EOF'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 1.0, 1.0); }
EOF
for _ in $(seq 80); do
  if tail -n +"$((mark + 1))" "$ANDROMEDA_LOG" | grep -q 'config reloaded'; then
    break
  fi
  sleep 0.05
done
if ! tail -n +"$((mark + 1))" "$ANDROMEDA_LOG" | grep -q 'config reloaded'; then
  echo "editing the shader file did not trigger configuration reload"
  exit 1
fi
spawn shader-reloaded
"$ANDROMEDA" clock-advance 120
sample
if ! (( red < 30 && green > 220 && blue > 220 )); then
  echo "new transition did not use edited shader source: $red $green $blue"
  exit 1
fi
echo "shader sampling, progress, completion, closing snapshot, and file reload verified"
