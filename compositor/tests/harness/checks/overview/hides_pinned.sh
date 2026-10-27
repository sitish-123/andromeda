#!/usr/bin/env bash
# Pinned windows are temporarily absent from the overview, both as live overlays and as scaled cards. Closing the
# overview restores the same pinned window, and switching workspaces afterwards proves that its pinned state survived.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/overview-pinned-client.log"
readonly BEFORE="$ANDROMEDA_RUNTIME_DIR/overview-pinned-before.png"
readonly OPEN="$ANDROMEDA_RUNTIME_DIR/overview-pinned-open.png"
readonly UNPINNED="$ANDROMEDA_RUNTIME_DIR/overview-pinned-unpinned.png"
readonly REPINNED="$ANDROMEDA_RUNTIME_DIR/overview-pinned-repinned.png"
readonly CLOSED="$ANDROMEDA_RUNTIME_DIR/overview-pinned-closed.png"
readonly SWITCHED="$ANDROMEDA_RUNTIME_DIR/overview-pinned-switched.png"

sample_rgb() {
  magick "$1" -crop 20x20+390+240 \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_blue_visible() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image")"
  if ((blue < 100 || blue < red + 30)); then
    echo "$label window is not visible: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_no_blue() {
  local image=$1 red green blue
  read -r red green blue <<< "$(sample_rgb "$image")"
  if ((red > 10 || green > 10 || blue > 10)); then
    echo "overview still shows pinned content or a pinned card: red=$red green=$green blue=$blue"
    return 1
  fi
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[output.HEADLESS-1]
workspaces = 2

[[window_rule]]
match.title = "^overview-pinned$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 120, y = 100, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

"$CLIENT" overview-pinned 420 260 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "pinned overview client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "overview-pinned") | .id')
if [[ -z $window_id ]]; then
  echo "could not resolve pinned overview client: $("$ANDROMEDA" windows --json)"
  exit 1
fi
"$ANDROMEDA" msg "window-focus:$window_id" > /dev/null
"$ANDROMEDA" msg window-toggle-pinned > /dev/null
sleep 0.1

grim "$BEFORE"
assert_blue_visible before-overview "$BEFORE"

"$ANDROMEDA" msg overview-open > /dev/null
sleep 0.1
grim "$OPEN"
assert_no_blue "$OPEN"

"$ANDROMEDA" msg window-toggle-pinned > /dev/null
sleep 0.1
grim "$UNPINNED"
assert_blue_visible after-unpin "$UNPINNED"

"$ANDROMEDA" msg window-toggle-pinned > /dev/null
sleep 0.1
grim "$REPINNED"
assert_no_blue "$REPINNED"

"$ANDROMEDA" msg overview-close > /dev/null
sleep 0.1
grim "$CLOSED"
assert_blue_visible after-overview "$CLOSED"

"$ANDROMEDA" msg workspace-switch:2 > /dev/null
sleep 0.1
grim "$SWITCHED"
assert_blue_visible after-workspace-switch "$SWITCHED"

echo "overview hides pinned windows completely and restores their pinned state on close"
