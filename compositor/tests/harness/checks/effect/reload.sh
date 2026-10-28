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

# reload-runtime: an IPC-only pool keeps its override, cached member and
# prepared programs through unrelated and effect-source reloads while suppressed.
sed -i 's/^screen = "tint"$/screen = ""/' "$ANDROMEDA_CONFIG"
cat > "$ANDROMEDA_RUNTIME_DIR/runtime-reload.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat > "$ANDROMEDA_RUNTIME_DIR/runtime-reload-overlay.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat >> "$ANDROMEDA_CONFIG" <<'EOF_CONFIG'
[effects.preset.runtime_reload_z]
kind = "border"
shader = "runtime-reload.glsl"
overlay = "runtime_reload_overlay"
[effects.preset.runtime_reload_a]
kind = "border"
shader = "runtime-reload.glsl"
[effects.preset.runtime_reload_overlay]
kind = "window"
shader = "runtime-reload-overlay.glsl"
[effects.pool.runtime_reload]
kind = "border"
choose = ["runtime_reload_z", "runtime_reload_a"]
selection = "round_robin"
EOF_CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
reload_id=$(jq -r .id <<< "$window")
"$ANDROMEDA" msg "effect-border-set:runtime_reload/$reload_id" > /dev/null
"$ANDROMEDA" msg "effect-border-toggle:$reload_id" > /dev/null
slot_before=$("$ANDROMEDA" windows --json | jq -c --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect')
sed -i 's/^accent_primary = "#FF0000FF"$/accent_primary = "#FFFFFFFF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
# Editing a member forces effects reconciliation, unlike the palette-only reload.
printf '\n// changed source\n' >> "$ANDROMEDA_RUNTIME_DIR/runtime-reload.glsl"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
slot_after=$("$ANDROMEDA" windows --json | jq -c --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect')
if [[ $slot_before != "$slot_after" ]] || ! jq -e '.name == "runtime_reload_z" and .pool == "runtime_reload" and .source == "runtime" and .suppressed' <<< "$slot_after" > /dev/null; then
  echo "reload-runtime-slot: suppressed override changed: $slot_before -> $slot_after"
  exit 1
fi
"$ANDROMEDA" effects --json | jq -e '[.presets[] | select(.name | startswith("runtime_reload_")) | .state] == ["compiled", "compiled", "compiled"]' > /dev/null || { echo "reload-runtime-prepared"; exit 1; }
"$ANDROMEDA" msg "effect-border-toggle:$reload_id" > /dev/null
"$ANDROMEDA" settle > /dev/null
grim "$IMAGE"
read -r r g b < <(probe)
if (( g < 240 || r > 15 || b > 15 )); then
  echo "reload-runtime-pixel: restored override did not render: $r $g $b"
  exit 1
fi
"$ANDROMEDA" windows --json | jq -e --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect | .name == "runtime_reload_z" and (.suppressed | not)' > /dev/null || { echo "reload-runtime-no-pick"; exit 1; }

read -r r g b < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y + 20))")
if (( r < 240 || g > 15 || b > 15 )); then
  echo "reload-runtime-overlay: overlay did not render after suppressed reload: $r $g $b"
  exit 1
fi
# Removing the held member repairs the assignment, retaining the pool override.
sed -i 's/^choose = \["runtime_reload_z", "runtime_reload_a"\]$/choose = ["runtime_reload_a"]/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" windows --json | jq -e --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect | .name == "runtime_reload_a" and .pool == "runtime_reload" and .source == "runtime" and (.suppressed | not)' > /dev/null || { echo "reload-removed-member"; exit 1; }
# Inactive member history is not itself a compilation root.
"$ANDROMEDA" effects --json | jq -e '.presets[] | select(.name == "runtime_reload_z") | .state == "unreferenced"' > /dev/null || { echo "reload-history-not-root"; exit 1; }
sed -i 's/^choose = \["runtime_reload_a"\]$/choose = ["runtime_reload_z", "runtime_reload_a"]/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" windows --json | jq -e --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect.name == "runtime_reload_a"' > /dev/null || { echo "reload-retains-valid-member"; exit 1; }
# Deleting a pool, or changing a plain preset to the wrong kind, drops only the
# invalid runtime selector and resolves the configured border default again.
sed -i 's/^\[effects.pool.runtime_reload\]$/[effects.pool.runtime_reload_removed]/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" windows --json | jq -e --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect | .name == "later" and .pool == "" and .source == "default"' > /dev/null || { echo "reload-deleted-override"; exit 1; }
"$ANDROMEDA" msg "effect-border-set:runtime_reload_z/$reload_id" > /dev/null
sed -i '/^\[effects.preset.runtime_reload_z\]$/,/^\[/s/^kind = "border"$/kind = "window"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" windows --json | jq -e --arg id "$reload_id" '.[] | select(.id == $id) | .border_effect | .name == "later" and .pool == "" and .source == "default"' > /dev/null || { echo "reload-wrong-kind-override"; exit 1; }

# An unrelated chrome refresh must preserve focus while repainting. Temporarily
# unfocusing the current window would consume an otherwise untouched rule pool.
cat > "$ANDROMEDA_RUNTIME_DIR/chrome-z.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$ANDROMEDA_RUNTIME_DIR/chrome-a.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat > "$ANDROMEDA_RUNTIME_DIR/chrome-focused.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0); }
GLSL
cat >> "$ANDROMEDA_CONFIG" <<'EOF_CHROME'
[effects.preset.chrome_z]
kind = "window"
shader = "chrome-z.glsl"
[effects.preset.chrome_a]
kind = "window"
shader = "chrome-a.glsl"
[effects.preset.chrome_focused]
kind = "window"
shader = "chrome-focused.glsl"
[effects.pool.chrome_idle]
kind = "window"
choose = [] # chrome pool starts inert during rule/chrome setup
selection = "round_robin"
[effects.pool.chrome_focus]
kind = "window"
choose = ["chrome_focused"]
[[window_rule]]
match.title = "^reload$"
window_effect = "chrome_idle"
[[window_rule]]
match.title = "^reload$"
match.is_focused = true
window_effect = "chrome_focus"
[[window_rule]]
match.title = "^chrome-helper$"
default_floating = true
default_position = { x = 800, y = 440, anchor = "top_left" }
EOF_CHROME
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" chrome-helper 200 160 > "$ANDROMEDA_RUNTIME_DIR/chrome-helper.log" 2>&1 &
for _ in $(seq 80); do
  chrome_helper=$("$ANDROMEDA" windows --json | jq -c '.[] | select(.title == "chrome-helper")')
  [[ -n $chrome_helper ]] && break
  sleep 0.025
done
[[ -n $chrome_helper ]]
chrome_id=$(jq -r .id <<< "$chrome_helper")
"$ANDROMEDA" msg "window-focus:$reload_id" > /dev/null
"$ANDROMEDA" settle > /dev/null
# This changes only effects, so the initially empty round-robin pool has never
# made a handout.
sed -i 's/^choose = \[\] # chrome pool starts inert during rule\/chrome setup$/choose = ["chrome_z", "chrome_a"]/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
chrome_before=$("$ANDROMEDA" windows --json | jq -c --arg id "$reload_id" '.[] | select(.id == $id) | .window_effect')
sed -i 's/^accent_primary = "#FFFFFFFF"$/accent_primary = "#808080FF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
chrome_after=$("$ANDROMEDA" windows --json | jq -c --arg id "$reload_id" '.[] | select(.id == $id) | .window_effect')
if [[ $chrome_before != "$chrome_after" ]] || ! jq -e '.name == "chrome_focused" and .pool == "chrome_focus"' <<< "$chrome_after" > /dev/null; then
  echo "reload-chrome-focused-stable: color reload changed the focused owner: $chrome_before -> $chrome_after"
  exit 1
fi
# Target an unfocused helper: choosing the untouched pool must not unfocus the
# first owner, and a fresh round-robin pick must still start at its first member.
"$ANDROMEDA" msg "effect-window-set:chrome_idle/$chrome_id" > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" windows --json | jq -e --arg id "$chrome_id" '.[] | select(.id == $id) | .window_effect | .name == "chrome_z" and .pool == "chrome_idle"' > /dev/null || { echo "reload-chrome-no-policy-pick"; exit 1; }
read -r cx cy cw ch < <("$ANDROMEDA" windows --json | jq -r --arg id "$chrome_id" '.[] | select(.id == $id) | "\(.x) \(.y) \(.w) \(.h)"')
grim "$IMAGE"
read -r r g b < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" pixel "$((cx + cw / 2))" "$((cy + ch / 2))")
if (( r < 240 || g > 15 || b > 15 )); then
  echo "reload-chrome-first-member-pixel: fresh owner did not draw the first member: $r $g $b"
  exit 1
fi
