#!/usr/bin/env bash
# harness: outputs=2
# Spaces overview on two outputs: dragging a window into the other output's main area moves it to that output.
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
output_x() {
  "$ANDROMEDA" outputs |
    awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}
"$ANDROMEDA_FRACTIONAL_CLIENT" card 480 300 > "$ANDROMEDA_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$ANDROMEDA" settle > /dev/null
source_output=$(title_bar_window card | jq -r '.output')
target_output=$([[ $source_output == HEADLESS-1 ]] && echo HEADLESS-2 || echo HEADLESS-1)
source_x=$(output_x "$source_output")
target_x=$(output_x "$target_output")
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Each output's main area centres on (640, 443); the lone Split window fills the source one.
pointer_hold 2560 720 move $((source_x + 640)) 443 press 272 move $((source_x + 640)) 483 \
  move $((target_x + 600)) 443 move $((target_x + 640)) 443 -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
landed=$(title_bar_window card | jq -r '.output')
[[ $landed == "$target_output" ]] || { echo "drop on $target_output left the window on $landed"; exit 1; }
echo "dragging a window into the other output's main area moved it to $target_output"
