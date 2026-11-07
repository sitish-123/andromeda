#!/usr/bin/env bash
# Free mode: resting the pointer on an SSD maximize button opens the Snap Layouts flyout under it, and a click on a
# zone snaps the window there (DESIGN.md "Free mode Snap Layouts").
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "forced"
height = 28
button_position = "right"
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^snap-flyout-hover$"
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" snap-flyout-hover 480 300 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box snap-flyout-hover 480x300+173+109
"$ANDROMEDA" settle > /dev/null
# Maximize sits 36 px left of the right edge in the 28 px title row above the content.
max_x=$((173 + 480 - 36)) max_y=$((109 - 14))
"$ANDROMEDA_POINTER_CLIENT" 1280 720 move "$max_x" "$max_y"
sleep 0.6 # real time: the 400 ms maximize dwell timer
# The 224 px sheet hangs 8 below the maximize button (title row 81..109), centred on it: the thirds mini (third in
# row one) has its middle zone's centre at sheet left + 8 + 2 * 72 + 33, sheet top + 8 + 20.
"$ANDROMEDA_POINTER_CLIENT" 1280 720 move "$((max_x - 112 + 185))" "$((109 + 8 + 28))" click 272
title_bar_wait_field snap-flyout-hover snap 3.2
echo "a rest on the maximize button opened the Snap Layouts flyout, and a click snapped to 3.2"
