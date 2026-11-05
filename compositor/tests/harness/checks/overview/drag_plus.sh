#!/usr/bin/env bash
# Spaces overview: dropping a window on the + thumb moves it to a new workspace.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[layout]
mode = "dwindle"
[overview]
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" card 480 300 > "$ANDROMEDA_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$ANDROMEDA" settle > /dev/null
before=$(title_bar_window card | jq -r '.workspace')
count=$("$ANDROMEDA" workspaces --json | jq 'length')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Spaces thumbs: 176 x 99 at y 20, 18 apart, centred, + after the last one.
plus_x=$(((1280 - (count + 1) * 176 - count * 18) / 2 + count * 194 + 88))
pointer_hold 1280 720 move 640 443 press 272 move 640 403 move "$plus_x" 80 move "$plus_x" 70 -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
after=$(title_bar_window card | jq -r '.workspace')
[[ $after != "$before" ]] || { echo "drop on + left the window on $before"; "$ANDROMEDA" workspaces --json; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
echo "a drop on + moved the window from $before to the new workspace $after"
