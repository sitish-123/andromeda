#!/usr/bin/env bash
# Layout policy controls placement without turning explicit floats into tiles on exit.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[layout]
mode = "floating"
new_exits_fullscreen = ["tiled"]
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[[window_rule]]
match.title = "^explicit$"
default_floating = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
spawn() {
  LOG_CONFIGURES=1 "$ANDROMEDA_FRACTIONAL_CLIENT" "$1" 480 300 > "$ANDROMEDA_RUNTIME_DIR/$1.log" 2>&1 &
  title_bar_wait_field "$1" floating true
}
focus() { "$ANDROMEDA" msg "window-focus-warp:$(title_bar_window "$1" | jq -r .id)" > /dev/null; }
layout() { "$ANDROMEDA" msg "workspace-set-layout:$1" > /dev/null; }
reject_toggle() {
  local reply
  if reply=$("$ANDROMEDA" msg "$1" 2>&1); then
    echo "$1 unexpectedly succeeded"
    return 1
  fi
  [[ $reply == *"window-toggle-floating needs a tiling layout"* ]]
}
spawn a
title_bar_wait_box a 480x300+400+210
first=$(rg -m 1 '^configure ' "$ANDROMEDA_RUNTIME_DIR/a.log")
[[ $first == 'configure 480x300' || $first == 'configure 0x0' ]]
spawn b
title_bar_wait_box b 480x300+432+242
reject_toggle window-toggle-floating
reject_toggle "window-toggle-floating:$(title_bar_window a | jq -r .id)"
focus b
"$ANDROMEDA" msg window-focus-left > /dev/null
title_bar_wait_field a focused true
"$ANDROMEDA" msg window-focus-down > /dev/null
title_bar_wait_field b focused true
spawn explicit
layout scrolling
title_bar_wait_field a floating false
title_bar_wait_field b floating false
title_bar_wait_field explicit floating true
layout floating
title_bar_wait_field a floating true
title_bar_wait_field b floating true
# Fullscreen survives both policy transitions.
focus a
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
title_bar_wait_field a fullscreen true
layout dwindle
title_bar_wait_field a floating false
title_bar_wait_field a fullscreen true
layout floating
title_bar_wait_field a floating true
title_bar_wait_field a fullscreen true
spawn incoming
title_bar_wait_field a fullscreen true
focus a
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
# A late identity rule changes placement intent, even though the window already floats.
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^late$"
match.content_type = "game"
default_floating = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
mkfifo "$ANDROMEDA_RUNTIME_DIR/late-control"
exec 9<> "$ANDROMEDA_RUNTIME_DIR/late-control"
CONTENT_TYPE_AFTER_MAP=game "$ANDROMEDA_UNMAP_CLIENT" late 480 300 <&9 > "$ANDROMEDA_RUNTIME_DIR/late.log" 2>&1 &
title_bar_wait_field late floating true
printf u >&9
await_lines "$ANDROMEDA_RUNTIME_DIR/late.log" 'content-type-updated' 1
layout master
title_bar_wait_field a floating false
title_bar_wait_field b floating false
title_bar_wait_field late floating true
title_bar_wait_field explicit floating true
# The standard cycle includes the fourth mode and wraps back to scrolling.
layout toggle
title_bar_wait_field a floating true
[[ $("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .layout') == floating ]]
layout toggle
title_bar_wait_field a floating false
[[ $("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .layout') == scrolling ]]
echo "floating layout placement, transitions, focus, and rules verified"
