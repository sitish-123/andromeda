#!/usr/bin/env bash
# Snap assist and snap groups (DESIGN.md "Free mode Snap Layouts"): after a Snap Layouts pick, the layout's empty zone
# offers the other window, Enter snaps it there, and the two form a group: restoring one from minimized restores both.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
readonly OUTPUT_W=1280 OUTPUT_H=720 K_ENTER=28
readonly FOREIGN_TOPLEVEL="${ANDROMEDA_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[[window_rule]]
match.title = "^assist-a$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^assist-b$"
default_floating = true
default_position = { x = 600, y = 300, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" assist-b 400 300 > "$ANDROMEDA_RUNTIME_DIR/assist-b.log" 2>&1 &
title_bar_wait_box assist-b 400x300+600+300
"$ANDROMEDA_FRACTIONAL_CLIENT" assist-a 400 300 > "$ANDROMEDA_RUNTIME_DIR/assist-a.log" 2>&1 &
title_bar_wait_box assist-a 400x300+100+100
pointer() { "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

# Mod+Z with the first zone lit: Enter snaps assist-a to the left half; assist then offers assist-b for the right.
"$ANDROMEDA" msg window-snap-layouts > /dev/null
pointer tap "$K_ENTER"
title_bar_wait_field assist-a snap 1.1
pointer tap "$K_ENTER"
title_bar_wait_field assist-b snap 1.2

# The group restores together: minimize both, restore assist-a through foreign-toplevel, assist-b comes back too.
OUTPUT=$(title_bar_window assist-a | jq -r .output)
"$ANDROMEDA" msg "window-focus:$(title_bar_window assist-a | jq -r .id)" > /dev/null
"$ANDROMEDA" msg window-minimize > /dev/null
"$ANDROMEDA" msg "window-focus:$(title_bar_window assist-b | jq -r .id)" > /dev/null
"$ANDROMEDA" msg window-minimize > /dev/null
title_bar_wait_field assist-a minimized true
title_bar_wait_field assist-b minimized true
"$FOREIGN_TOPLEVEL" assist-a "$OUTPUT" activate > /dev/null
title_bar_wait_field assist-a minimized false
title_bar_wait_field assist-b minimized false
echo "snap assist offered the other window for the empty zone, and the snap group restored together"
