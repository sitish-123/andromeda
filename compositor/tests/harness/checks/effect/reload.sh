#!/usr/bin/env bash
# Reload behaviour of effects: a missing shader renders plainly and recovers once the file appears; a [colors] change
# reaches a palette shader without a recompile; the light layer goes away with the last lit preset and comes back with
# a new one; a [colors] change reaches a screen palette shader that does not read andromeda_time.
set -euo pipefail
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/effect-reload.png"
readonly LOG_MARK=$(($(wc -l < "$ANDROMEDA_LOG") + 1))
cat > "$ANDROMEDA_RUNTIME_DIR/palette.glsl" <<'GLSL'
// andromeda_scale keeps the palette index a runtime value (always 0 here) rather than a shader-compile-time constant.
vec4 border(vec2 uv) { return andromeda_palette_at(andromeda_scale * 1e-9); }
GLSL
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 6
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
accent_primary = "#00FF00FF"
[colors.border]
focused = "#FFFFFFFF"
[effects]
border = "later"
[effects.preset.later]
kind = "border"
shader = "later.glsl"
[[window_rule]]
match.title = "^reload$"
default_floating = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null
for _ in $(seq 50); do
  grep -q "config reloaded" <(tail -n +"$LOG_MARK" "$ANDROMEDA_LOG") && break
  sleep 0.02
done
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" reload 300 200 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$ANDROMEDA" windows --json | jq -c '.[] | select(.title == "reload")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$ANDROMEDA" settle > /dev/null
read -r x y w _ < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
probe() { "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y - 3))"; }
grim "$IMAGE"
read -r r g b < <(probe)
if (( r < 240 || g < 240 || b < 240 )); then
  echo "an inert preset did not leave the plain white ring: $r $g $b"
  exit 1
fi
# The missing file appears: the watcher reloads and the ring turns accent_primary green.
cp "$ANDROMEDA_RUNTIME_DIR/palette.glsl" "$ANDROMEDA_RUNTIME_DIR/later.glsl"
sed -i 's/^shader = "later.glsl"$/shader = "later.glsl"\npalette = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
grim "$IMAGE"
read -r r g b < <(probe)
if (( g < 240 || r > 15 || b > 15 )); then
  echo "the preset did not recover once its shader file appeared: $r $g $b"
  exit 1
fi
# A [colors] change updates the palette uniform without recompiling.
compiles=$(tail -n +"$LOG_MARK" "$ANDROMEDA_LOG" | grep -c "Compiling border shader" || true)
sed -i 's/^accent_primary = "#00FF00FF"$/accent_primary = "#0000FFFF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
grim "$IMAGE"
read -r r g b < <(probe)
if (( b < 240 || g > 15 )); then
  echo "a colour change did not reach the palette uniform: $r $g $b"
  exit 1
fi
if (( $(tail -n +"$LOG_MARK" "$ANDROMEDA_LOG" | grep -c "Compiling border shader" || true) != compiles )); then
  echo "a colour change recompiled the preset"
  exit 1
fi
# Light spills blue above the ring while the preset has a light table, and stops once a reload removes it; a further
# reload that adds it back restores the light.
glow() { "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y - 20))"; }
add_light() {
  printf '\n[effects.preset.later.light]\nspread = 40\nintensity = 4\nthreshold = 0.2\n' >> "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" settle > /dev/null
  grim "$IMAGE"
  read -r _ _ b < <(glow)
  if (( b < 15 )); then
    echo "$1: the lit preset spilled no light above the ring: blue=$b"
    exit 1
  fi
}
add_light "first light"
sed -i '/^\[effects\.preset\.later\.light\]$/,$d' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
grim "$IMAGE"
read -r _ _ b < <(glow)
if (( b > 4 )); then
  echo "light stayed after the reload removed the last lit preset: blue=$b"
  exit 1
fi
add_light "light added back"
# A screen program that does not read andromeda_time gets no effect frames, so only the reload can repaint it.
sed 's/vec4 border/vec4 screen/' "$ANDROMEDA_RUNTIME_DIR/palette.glsl" > "$ANDROMEDA_RUNTIME_DIR/tint.glsl"
sed -i '/^\[effects\]$/a screen = "tint"\nin_capture = true' "$ANDROMEDA_CONFIG"
printf '\n[effects.preset.tint]\nkind = "screen"\nshader = "tint.glsl"\npalette = true\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
screen() { "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel 20 20; }
grim "$IMAGE"
read -r r g b < <(screen)
if (( b < 240 || r > 15 || g > 15 )); then
  echo "the screen palette preset did not paint accent_primary: $r $g $b"
  exit 1
fi
sed -i 's/^accent_primary = "#0000FFFF"$/accent_primary = "#FF0000FF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
grim "$IMAGE"
read -r r g b < <(screen)
if (( r < 240 || g > 15 || b > 15 )); then
  echo "a color change did not reach the screen palette uniform: $r $g $b"
  exit 1
fi
echo "inert preset recovery, palette updates without recompilation, light layer reloads, and screen palette updates" \
  "verified"
