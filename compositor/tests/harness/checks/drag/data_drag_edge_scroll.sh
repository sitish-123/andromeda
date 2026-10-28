#!/usr/bin/env bash
# A client data-device drag held at the scrolling edge must move the strip far enough to reveal an offscreen target.
# Moving inward over that newly revealed client and releasing must complete a real Wayland drop.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly DRAG_CLIENT="${ANDROMEDA_DRAG_CLIENT:-./build-debug/tests/drag-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SOURCE_LOG="$ANDROMEDA_RUNTIME_DIR/data-drag-edge-source.log"
readonly TARGET_LOG="$ANDROMEDA_RUNTIME_DIR/data-drag-edge-target.log"

window_count() {
  "$ANDROMEDA" windows --json | jq 'length'
}

wait_for_count() {
  local want=$1
  for _ in $(seq 80); do
    [[ $(window_count) -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$ANDROMEDA" windows --json)"
  return 1
}

wait_for_log() {
  local file=$1 pattern=$2 label=$3
  for _ in $(seq 80); do
    grep -q "$pattern" "$file" 2>/dev/null && return 0
    sleep 0.05
  done
  echo "$label did not report '$pattern': $(< "$file")"
  return 1
}

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 624 700 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client data-drag-edge-a
wait_for_count 1
spawn_client data-drag-edge-b
wait_for_count 2
spawn_client data-drag-edge-c
wait_for_count 3
"$DRAG_CLIENT" target data-drag-edge-target > "$TARGET_LOG" 2>&1 &
TARGET_PID=$!
wait_for_log "$TARGET_LOG" '^ready$' "drop target"
wait_for_count 4

"$ANDROMEDA" msg column-focus-first > /dev/null
"$ANDROMEDA" settle

windows=$("$ANDROMEDA" windows --json)
initial_first_x=$(jq -r '.[] | select(.title == "data-drag-edge-a") | .x' <<< "$windows")
initial_target_x=$(jq -r '.[] | select(.title == "data-drag-edge-target") | .x' <<< "$windows")
if ((initial_target_x < OUTPUT_W)); then
  echo "drop target began inside the viewport instead of offscreen: $windows"
  exit 1
fi

"$DRAG_CLIENT" > "$SOURCE_LOG" 2>&1 &
SOURCE_PID=$!
wait_for_log "$SOURCE_LOG" '^ready$' "drag source"

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move 32 32 press "$LEFT_BUTTON" \
  -- move 1279 360 mark edge hold move 1000 360 mark target hold release "$LEFT_BUTTON"
wait_for_log "$SOURCE_LOG" '^drag-started$' "drag source"

pointer_step edge
moved=false
first_x=$initial_first_x
target_x=$initial_target_x
for _ in $(seq 120); do
  windows=$("$ANDROMEDA" windows --json)
  first_x=$(jq -r '.[] | select(.title == "data-drag-edge-a") | .x' <<< "$windows")
  target_x=$(jq -r '.[] | select(.title == "data-drag-edge-target") | .x' <<< "$windows")
  if ((first_x < initial_first_x && target_x <= 700)); then
    moved=true
    break
  fi
  sleep 0.05
done
if [[ $moved != true ]]; then
  echo "data-device drag at the right edge did not reveal the offscreen target: first x $initial_first_x -> $first_x, target x $initial_target_x -> $target_x"
  exit 1
fi
wait_for_log "$TARGET_LOG" '^drag-enter$' "stationary edge drop target"

pointer_step target
pointer_release

wait_for_log "$TARGET_LOG" '^drop-received$' "drop target"
wait_for_log "$SOURCE_LOG" '^drag-finished$' "drag source"
wait "$TARGET_PID"
wait "$SOURCE_PID"

echo "data-device drag scrolled to an offscreen window and completed the drop"
