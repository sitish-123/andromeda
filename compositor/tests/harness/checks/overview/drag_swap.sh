#!/usr/bin/env bash
# Grid overview: dropping a window on the middle of a window in another workspace swaps the two across workspaces;
# a plain move would leave the target where it was.
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
style = "grid"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" first 480 300 > "$ANDROMEDA_RUNTIME_DIR/first.log" 2>&1 &
title_bar_wait_field first floating false
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" second 480 300 > "$ANDROMEDA_RUNTIME_DIR/second.log" 2>&1 &
title_bar_wait_field second floating false
"$ANDROMEDA" settle > /dev/null
ws_of() { title_bar_window "$1" | jq -r '.workspace'; }
first_ws=$(ws_of first)
second_ws=$(ws_of second)
[[ $first_ws != "$second_ws" ]] || { echo "expected two workspaces, both on $first_ws"; exit 1; }
count=$("$ANDROMEDA" workspaces --json | jq 'length')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Grid tiles (style.cpp placeGrid on 1280 x 720): 368 wide, 28 apart, centred; one tile per workspace plus the + cell.
read -r ax ay bx by < <(awk -v n="$count" 'BEGIN {
  cols = n + 1 < 3 ? n + 1 : 3; rows = int((n + cols) / cols); tw = 368; th = 207; avail = 610
  x0 = (1280 - (cols * tw + (cols - 1) * 28)) / 2; pad = (avail - (rows * th + (rows - 1) * 52)) / 2
  y0 = 64 + (pad > 0 ? pad : 0)
  printf "%d %d %d %d\n", x0 + tw / 2, y0 + th / 2, x0 + tw + 28 + tw / 2, y0 + th / 2 }')
pointer_hold 1280 720 move "$ax" "$ay" press 272 move "$ax" $((ay + 40)) move $((bx - 20)) "$by" move "$bx" "$by" \
  -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
[[ $(ws_of first) == "$second_ws" && $(ws_of second) == "$first_ws" ]] ||
  { echo "no swap: first on $(ws_of first), second on $(ws_of second) (was $first_ws, $second_ws)"; exit 1; }
echo "a drop on the middle of a window in another workspace swapped the two"
