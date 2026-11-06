#!/usr/bin/env bash
# Spaces overview: dragging the second workspace's thumb onto the first one moves that workspace to the front.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[overview]
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" first 480 300 > "$ANDROMEDA_RUNTIME_DIR/first.log" 2>&1 &
title_bar_wait_field first floating false
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" second 480 300 > "$ANDROMEDA_RUNTIME_DIR/second.log" 2>&1 &
title_bar_wait_field second floating false
"$ANDROMEDA" settle > /dev/null
# A window's workspace field is the stable id; the position is that workspace's index.
slot() {
  "$ANDROMEDA" workspaces --json | jq -r --arg id "$(title_bar_window "$1" | jq -r '.workspace')" \
    '.[] | select(.id == $id) | .index'
}
[[ $(slot first) == 1 && $(slot second) == 2 ]] || { echo "expected first on 1, second on 2"; exit 1; }
count=$("$ANDROMEDA" workspaces --json | jq 'length')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Spaces thumbs (style.cpp placeBar on 1280 x 720): 176 x 99 at y 20, 18 apart, centred, + after the last one.
x0=$(((1280 - (count + 1) * 176 - count * 18) / 2 + 88))
pointer_hold 1280 720 move $((x0 + 194)) 69 press 272 move $((x0 + 160)) 69 move $((x0 + 10)) 69 move "$x0" 69 \
  -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
[[ $(slot second) == 1 && $(slot first) == 2 ]] ||
  { echo "no reorder: first on $(slot first), second on $(slot second)"; exit 1; }
echo "dragging the second thumb onto the first moved that workspace to the front"
