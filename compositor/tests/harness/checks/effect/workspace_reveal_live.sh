#!/usr/bin/env bash
# Both clients must repaint after frame callbacks while reveal progress is held.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/reveal.png"
workspace_reveal_config
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[[window_rule]]
match.title = "^reveal-"
default_floating = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
mkfifo "$ANDROMEDA_RUNTIME_DIR/a.fifo" "$ANDROMEDA_RUNTIME_DIR/b.fifo"
exec {a_fd}<>"$ANDROMEDA_RUNTIME_DIR/a.fifo"
exec {b_fd}<>"$ANDROMEDA_RUNTIME_DIR/b.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" reveal-a 600 500 \
  <&"$a_fd" > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/a.log" mapped 1
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle
REPAINT_ON_STDIN=1 FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" reveal-b 400 600 \
  <&"$b_fd" > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/b.log" mapped 1
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle

pixel() { "$ANDROMEDA_PIXEL_PROBE" "$SHOT" pixel "$1" "$2"; }
expect_pixel() {
  local x=$1 y=$2 want=$3 value
  value=$(pixel "$x" "$y")
  [[ $value == "$want" ]] || { echo "pixel $x,$y: expected $want, got $value"; return 1; }
}
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 500
# Default vertical axis: incoming at the top, outgoing at the bottom. Both
# floating clients cover these positions at rest; a half slide would not.
grim "$SHOT"
expect_pixel 640 200 '0 0 255'
expect_pixel 640 520 '255 0 0'

# Reject a failed reveal frame and keep native navigation at the held progress.
"$ANDROMEDA" animation-capture-fail on
"$ANDROMEDA" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 0 255'
[[ $("$ANDROMEDA" windows --json | jq -r '.[] | select(.active) | .title') == reveal-b ]]
"$ANDROMEDA" animation-capture-fail off
"$ANDROMEDA" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 0 255'
# The next native transition may reveal again.
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 1000
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '0 0 255'
expect_pixel 640 520 '255 0 0'

printf G >&"$a_fd"
await_lines "$ANDROMEDA_RUNTIME_DIR/a.log" repainted 1
for _ in $(seq 60); do
  grim "$SHOT"
  [[ $(pixel 640 520) == '0 255 0' ]] && break
  sleep 0.025
done
expect_pixel 640 520 '0 255 0'
expect_pixel 640 200 '0 0 255'
printf R >&"$b_fd"
await_lines "$ANDROMEDA_RUNTIME_DIR/b.log" repainted 1
for _ in $(seq 60); do
  grim "$SHOT"
  [[ $(pixel 640 200) == '255 0 0' ]] && break
  sleep 0.025
done
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'

# The outgoing view is visual only even where its revealed pixels remain.
"$ANDROMEDA_POINTER_CLIENT" 1280 720 move 640 520 click 272
"$ANDROMEDA" workspaces --json | jq -e '.[] | select(.active) | .name == "2"' > /dev/null
"$ANDROMEDA" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '255 0 0'

# Reverse direction and interrupt at half progress with a native rebase.
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 1000
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '255 0 0'
# Fullscreen uses a separate scene root but must share the same reveal edge.
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
b_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "reveal-b") | .id')
"$ANDROMEDA" msg "window-focus-warp:$b_id" > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'
# Disabling the event at held progress removes bindings and finishes at native active workspace 1.
sed -i '/^\[animation.workspaces\]$/a enabled = false' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '0 255 0'
expect_pixel 640 520 '0 255 0'
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle

# Horizontal reveal and renderer replacement at held progress.
sed -i '/^\[animation.workspaces\]$/{n; /enabled = false/d;}' "$ANDROMEDA_CONFIG"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[output."HEADLESS-1"]
workspace_axis = "horizontal"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 500
grim "$SHOT"
expect_pixel 500 360 '255 0 0'
expect_pixel 780 360 '0 255 0'
"$ANDROMEDA" renderer-recover > /dev/null
await_lines "$ANDROMEDA_LOG" '.*renderer recreated' 1
# Supply a fresh buffer after renderer replacement.
printf R >&"$b_fd"
await_lines "$ANDROMEDA_RUNTIME_DIR/b.log" repainted 2
"$ANDROMEDA" clock-advance 1
grim "$SHOT"
expect_pixel 500 360 '255 0 0'
expect_pixel 780 360 '255 0 0'
# A new reveal uses the replacement renderer; an empty endpoint leaves no source pixels.
"$ANDROMEDA" msg workspace-switch:3 > /dev/null
"$ANDROMEDA" clock-advance 1000
grim "$SHOT"
[[ $(pixel 640 360) != '255 0 0' ]]
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle

echo 'live frame-paced clients, resting presentation, different bounds, fullscreen, reverse direction, native rebase, focus and disable cleanup verified'
