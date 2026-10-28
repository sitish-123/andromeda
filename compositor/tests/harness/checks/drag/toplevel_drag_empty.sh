#!/usr/bin/env bash
# Releasing a carried window over empty space is a tear-off, not a cancel:
# the source must see dnd_finished, and the window must stay and be placed.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'
[animation]
enabled = false
[[window_rule]]
match.title = "^toplevel-drag-source$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

source_log="$ANDROMEDA_RUNTIME_DIR/toplevel-empty-source.log"
fifo="$ANDROMEDA_RUNTIME_DIR/toplevel-empty.fifo"
mkfifo "$fifo"
"$ANDROMEDA_DRAG_CLIENT" toplevel detached < "$fifo" > "$source_log" 2>&1 &
source_pid=$!
exec {control_fd}> "$fifo"
await_lines "$source_log" ready 1
"$ANDROMEDA" settle > /dev/null
pointer_hold 1280 720 move 200 200 press 272 -- move 400 300 mark moved hold move 1000 500 mark empty hold release 272
await_lines "$source_log" tear-off-mapped 1
pointer_step moved
pointer_step empty
pointer_release
await_lines "$source_log" drag-finished 1
grep -q drag-cancelled "$source_log" && { echo 'empty-space drop cancelled the drag'; exit 1; }
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" windows --json | jq -e 'any(.[]; .title == "toplevel-drag-window" and .floating == false)' > /dev/null \
  || { echo 'torn-off window was not placed'; exit 1; }
printf 'dq\n' >&"$control_fd"
wait "$source_pid"
exec {control_fd}>&-
echo 'empty-space drop completes the tear-off'
