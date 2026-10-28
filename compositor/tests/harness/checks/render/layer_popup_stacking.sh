#!/usr/bin/env bash
# A layer popup crosses a later bar, follows its parent, and stays below higher shell layers.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER="$ANDROMEDA_LAYER_CLIENT"
readonly POINTER="$ANDROMEDA_POINTER_CLIENT"
readonly PANEL_LOG="$ANDROMEDA_RUNTIME_DIR/panel.log"
readonly SIBLING_LOG="$ANDROMEDA_RUNTIME_DIR/sibling.log"
readonly OVERLAY_LOG="$ANDROMEDA_RUNTIME_DIR/overlay.log"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/popup.png"
readonly YELLOW='r > 0.95 && g > 0.6 && g < 0.75 && b < 0.05'

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/panel.fifo"
env LAYER_CONTROL=1 "$LAYER" HEADLESS-1 40 popup-on-release \
  < "$ANDROMEDA_RUNTIME_DIR/panel.fifo" > "$PANEL_LOG" 2>&1 &
panel_pid=$!
exec {panel_fd}> "$ANDROMEDA_RUNTIME_DIR/panel.fifo"
await_events "$PANEL_LOG" ready 1 "the parent bar"

"$POINTER" 1280 720 move 80 20 click 272 > /dev/null
await_events "$PANEL_LOG" popup-mapped 1 "the layer popup"
"$ANDROMEDA" settle
grim "$IMAGE"
visible=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the initial popup did not render fully: expected 6400 yellow pixels, got $visible"
  exit 1
fi

"$LAYER" HEADLESS-1 40 > "$SIBLING_LOG" 2>&1 &
await_events "$SIBLING_LOG" ready 1 "the later sibling bar"
"$ANDROMEDA" settle
grim "$IMAGE"
visible=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the sibling bar covered the popup: expected 6400 yellow pixels, got $visible"
  exit 1
fi

printf 'margins moved 30 0 0 0\n' >&"$panel_fd"
await_events "$PANEL_LOG" 'done moved' 1 "the moved parent bar"
"$ANDROMEDA" settle
grim "$IMAGE"
read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$YELLOW")
if ((x != 80 || y != 51 || w != 80 || h != 80)); then
  echo "the popup did not follow its parent: got $x,$y ${w}x$h"
  exit 1
fi

"$LAYER" HEADLESS-1 40 overlay-layer > "$OVERLAY_LOG" 2>&1 &
overlay_pid=$!
await_events "$OVERLAY_LOG" ready 1 "the higher overlay surface"
"$ANDROMEDA" settle
grim "$IMAGE"
visible=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 0)); then
  echo "the top-layer popup escaped above the overlay: $visible yellow pixels"
  exit 1
fi
kill "$overlay_pid"
wait "$overlay_pid" 2>/dev/null || true
"$ANDROMEDA" settle
grim "$IMAGE"
visible=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the popup did not reappear after the overlay closed: $visible yellow pixels"
  exit 1
fi

kill "$panel_pid"
wait "$panel_pid" 2>/dev/null || true
"$ANDROMEDA" settle
grim "$IMAGE"
visible=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 0)); then
  echo "the destroyed parent's popup remained visible: $visible yellow pixels"
  exit 1
fi
echo "layer popups cover sibling bars, follow their parent, respect higher layers, and close with their parent"
