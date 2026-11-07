#!/usr/bin/env bash
# The Snap Layouts flyout (window-snap-layouts, Mod+Z): Right then Enter snaps to the right half from the keyboard, and
# a click on a drawn zone snaps the window there (DESIGN.md "Free mode Snap Layouts").
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
readonly OUTPUT_W=1280 OUTPUT_H=720 BTN_LEFT=272 K_RIGHT=106 K_ENTER=28
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[[window_rule]]
match.title = "^snap-flyout$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" snap-flyout 480 300 > "$ANDROMEDA_RUNTIME_DIR/snap-flyout.log" 2>&1 &
title_bar_wait_box snap-flyout 480x300+173+109
pointer() { "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

"$ANDROMEDA" msg window-snap-layouts > /dev/null
pointer tap "$K_RIGHT" tap "$K_ENTER"
title_bar_wait_field snap-flyout snap 1.2
title_bar_wait_box snap-flyout 628x704+644+8
"$ANDROMEDA" msg window-snap:1.2 > /dev/null
title_bar_wait_box snap-flyout 480x300+173+109

# The sheet's top centre sits 8 px into the window's top centre (413, 117): 260 wide, 12 px padding, 72 x 44 minis
# 8 apart. Layout 3 (thirds) is the third mini; its middle zone's centre is at (491, 151).
"$ANDROMEDA" msg window-snap-layouts > /dev/null
pointer move 491 151 click "$BTN_LEFT"
title_bar_wait_field snap-flyout snap 3.2
title_bar_wait_box snap-flyout 414x704+433+8
echo "the Snap Layouts flyout snaps from the keyboard (1.2) and from a click (3.2)"
