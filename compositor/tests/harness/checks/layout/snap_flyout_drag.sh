#!/usr/bin/env bash
# Free mode: dragging a window's title bar to the top edge shows the Snap Layouts flyout, and dropping on a zone snaps
# the window there (DESIGN.md "Free mode Snap Layouts").
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[input]
window_snapping = true
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "forced"
[[window_rule]]
match.title = "^snap-flyout-drag$"
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" snap-flyout-drag 480 300 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box snap-flyout-drag 480x300+173+109
"$ANDROMEDA" settle > /dev/null
# The flyout opens at the top centre of the 1280 px output, 8 px down: its third mini (thirds) sits at (680, 16) and
# its middle zone's centre at (712, 36).
pointer_hold 1280 720 move 300 95 press 272 move 640 2 move 700 30 move 712 36 -- release 272
pointer_release
title_bar_wait_field snap-flyout-drag snap 3.2
echo "a title bar dragged to the top edge dropped on a Snap Layouts zone (3.2)"
