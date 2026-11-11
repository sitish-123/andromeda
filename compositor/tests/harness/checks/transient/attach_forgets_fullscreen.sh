#!/usr/bin/env bash
# A fullscreen tile that is floated remembers to return to fullscreen when it tiles again. Once its client makes it a
# modal dialog, it attaches to its parent and forgets that: tiled again after the client clears its parent, it stays
# out of fullscreen.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly CONTROL_FIFO="$ANDROMEDA_RUNTIME_DIR/attach-forgets-control"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/attach-forgets.log"

printf '\n[animation]\nenabled = false\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
"$CLIENT" adopt adopt <&"$control_fd" > "$LOG" 2>&1 &
await_events "$LOG" child 1 "the second window"
"$ANDROMEDA" settle

child() { "$ANDROMEDA" windows --json | jq -r --arg field "$1" '.[] | select(.title == "adopt-child") | .[$field]'; }
expect() {
  local field=$1 want=$2 step=$3
  if [[ $(child "$field") != "$want" ]]; then
    echo "$step: expected $field=$want: $("$ANDROMEDA" windows --json | jq -c '[.[] | {title, floating, fullscreen, modal, parent}]')"
    exit 1
  fi
}
focus_child() {
  "$ANDROMEDA" msg "window-focus:$(child id)" > /dev/null
  "$ANDROMEDA" settle
}

focus_child
expect floating false "the window did not open as a tile"
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
expect fullscreen true "the tile did not go fullscreen"
"$ANDROMEDA" msg window-toggle-floating > /dev/null
"$ANDROMEDA" settle
expect fullscreen false "floating did not drop fullscreen"
expect floating true "the fullscreen tile did not float"

echo parent >&"$control_fd"
await_events "$LOG" parented 1 "the parent request"
"$ANDROMEDA" settle
expect modal true "the window did not become a modal dialog"
[[ -n $(child parent) ]] || { echo "the dialog has no parent"; exit 1; }

echo orphan >&"$control_fd"
await_events "$LOG" orphaned 1 "the orphan request"
"$ANDROMEDA" settle
expect parent "" "the window kept its parent"

focus_child
"$ANDROMEDA" msg window-toggle-floating > /dev/null
"$ANDROMEDA" settle
expect floating false "the detached window did not tile"
expect fullscreen false "the window tiled back into the fullscreen it had before it attached"

echo "a floated fullscreen tile that attaches as a dialog forgets its fullscreen and tiles back windowed"
