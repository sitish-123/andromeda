#!/usr/bin/env bash
# Clear-colored title bars must be redrawn above blur after overview and focus changes.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'
[animation]
enabled = true
[appearance]
border_width = 2
corner_radius = 0
[appearance.title_bar]
mode = "forced"
[appearance.blur]
enabled = true
optimized = true
[colors]
backdrop = "#000000FF"
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#FF00FF80"
[[window_rule]]
match.title = "^blur-refocus-"
blur = true
EOF
"$ANDROMEDA" config validate >/dev/null
"$ANDROMEDA" msg config-reload >/dev/null
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 >"$ANDROMEDA_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/wallpaper.log" ready 1
for name in one two; do
  "$ANDROMEDA_UNMAP_CLIENT" "blur-refocus-$name" 480 300 >"$ANDROMEDA_RUNTIME_DIR/$name.log" 2>&1 &
  await_lines "$ANDROMEDA_RUNTIME_DIR/$name.log" mapped 1
done

expect_background() {
  local phase=$1 window x y color
  "$ANDROMEDA" settle >/dev/null
  window=$("$ANDROMEDA" windows --json | jq -c '.[] | select(.active)')
  [[ $(jq -r '.title_bar' <<< "$window") == true ]]
  x=$(jq -r '.x + 180' <<< "$window")
  y=$(jq -r '.y - 14' <<< "$window")
  grim -o HEADLESS-1 "$ANDROMEDA_RUNTIME_DIR/title-bar.png"
  color=$("$ANDROMEDA_PIXEL_PROBE" "$ANDROMEDA_RUNTIME_DIR/title-bar.png" pixel "$x" "$y")
  [[ $color == "0 0 0" ]] || {
    echo "$phase: focused title bar lost its opaque background: $color"
    exit 1
  }
}

ids=$("$ANDROMEDA" windows --json | jq -r '.[].id')
for id in $ids; do
  "$ANDROMEDA" msg "window-focus:$id" >/dev/null
  expect_background "before overview"
  "$ANDROMEDA" msg overview-open >/dev/null
  "$ANDROMEDA" settle >/dev/null
  "$ANDROMEDA" msg overview-close >/dev/null
  expect_background "after overview"
done
for id in $ids; do
  "$ANDROMEDA" msg "window-focus:$id" >/dev/null
  expect_background "after IPC refocus"
done
echo "opaque title bars retain their background across overview and IPC refocus with blur"
