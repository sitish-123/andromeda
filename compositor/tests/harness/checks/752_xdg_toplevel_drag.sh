#!/usr/bin/env bash
# xdg_toplevel_drag_manager_v1 is offered, and a window torn off during a data-device drag floats under the pointer
# at the offset its client gave, is never a drop target itself, and stays where the drop left it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/toplevel-drag.log"
readonly LEFT_BUTTON=272

cat >> "$UMBRIEL_CONFIG" <<'CONF'

[animation]
enabled = false
CONF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" toplevel-drag tabs > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the tab window"
"$UMBRIEL" settle
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "tabs") | "\(.x) \(.y) \(.w) \(.h)"')

torn() { "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "tabs-torn") | "\(.x) \(.y) \(.floating)"'; }
expect_torn_at() {
  local position
  for _ in $(seq 40); do
    position=$(torn)
    [[ $position == "$1 $2 true" ]] && return 0
    sleep 0.05
  done
  echo "the torn-off window is at '$position', not floating at $1,$2 $3"
  return 1
}

pointer_hold 1280 720 move $((x + w / 2)) $((y + h / 2)) press "$LEFT_BUTTON" -- \
  move 400 300 mark at-400 hold move 700 450 mark at-700 hold release "$LEFT_BUTTON" mark released
await_events "$LOG" dragging 1 "the drag"
await_events "$LOG" torn 1 "the torn-off window"
pointer_step at-400
expect_torn_at 380 290 "under the pointer"
pointer_step at-700
expect_torn_at 680 440 "after the pointer moved"
pointer_release
await_events "$LOG" drag-ended 1 "the end of the drag"
expect_torn_at 680 440 "after the drop"
if grep -q '^enter tabs-torn$' "$LOG"; then
  echo "the drag entered the window it carries"
  exit 1
fi
echo "torn-off window followed the pointer to 680,440, stayed there after the drop, and was never a drop target"
