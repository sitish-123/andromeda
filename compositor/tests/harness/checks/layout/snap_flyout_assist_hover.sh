#!/usr/bin/env bash
# A click on a Snap Layouts zone, from the flyout that a rest on the SSD maximize button opened, with another window
# on the workspace: the window snaps, snap assist offers the other window, and Enter snaps it too. The other window's
# app icon loads on a worker after assist first draws, so assist redraws its cards while they show title bars.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
readonly K_ENTER=28
data="$ANDROMEDA_RUNTIME_DIR/xdg-data"
mkdir -p "$data/applications" "$data/icons/hicolor/scalable/apps"
printf '[Desktop Entry]\nType=Application\nName=Assist Icon\nExec=true\nIcon=assist-icon-test\n' \
  > "$data/applications/assist.icon.test.desktop"
cat > "$data/icons/hicolor/scalable/apps/assist-icon-test.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#ff00ff"/></svg>
SVG
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
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
match.title = "^assist-hover-a$"
default_position = { x = 173, y = 109, anchor = "top_left" }
[[window_rule]]
match.title = "^assist-hover-b$"
default_position = { x = 700, y = 300, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
APP_ID=assist.icon.test "$ANDROMEDA_UNMAP_CLIENT" assist-hover-b 400 300 > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
title_bar_wait_box assist-hover-b 400x300+700+300
"$ANDROMEDA_FRACTIONAL_CLIENT" assist-hover-a 480 300 > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
title_bar_wait_box assist-hover-a 480x300+173+109
"$ANDROMEDA" settle > /dev/null
pointer() { "$ANDROMEDA_POINTER_CLIENT" 1280 720 "$@"; }
# Maximize sits 36 px left of the right edge in the 28 px title row above the content.
max_x=$((173 + 480 - 36)) max_y=$((109 - 14))
pointer move "$max_x" "$max_y"
sleep 0.6 # real time: the 400 ms maximize dwell timer
"$ANDROMEDA" settle > /dev/null
# The 224 px sheet hangs 8 below the title row, centred on the button: the halves mini (first in row one) has its left
# zone's centre at sheet left + 8 + 15, sheet top + 8 + 20.
pointer move "$((max_x - 112 + 23))" "$((109 + 8 + 28))" click 272
title_bar_wait_field assist-hover-a snap 1.1
"$ANDROMEDA" settle > /dev/null
pointer tap "$K_ENTER"
title_bar_wait_field assist-hover-b snap 1.2
echo "a hover-flyout zone click snapped the window, and snap assist snapped the other one"
