#!/usr/bin/env bash
# Strip overview: holding a dragged card at the bottom edge steps the rows down under it, and the drop lands on the
# row that slid under the pointer.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[overview]
style = "strip"
zoom = 0.5
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" card 480 300 > "$ANDROMEDA_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$ANDROMEDA" settle > /dev/null
active() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .name'; }
[[ $(active) == 1 ]] || { echo "expected workspace 1 active, got $(active)"; exit 1; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
read -r x y < <(title_bar_window card | jq -r '"\(320 + (.x + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
pointer_hold 1280 720 move "$x" "$y" press 272 move "$x" $((y + 40)) move 640 600 move 640 705 -- move 640 360 \
  pause 100 release 272
for _ in $(seq 60); do
  [[ $(active) == 2 ]] && break
  sleep 0.05
done
[[ $(active) == 2 ]] || { echo "holding at the bottom edge did not step to workspace 2 (active $(active))"; exit 1; }
pointer_release
"$ANDROMEDA" settle > /dev/null
title_bar_wait_field card workspace "$(title_bar_window card | jq -r '.workspace | split(":")[0]'):2"
"$ANDROMEDA" msg overview-close > /dev/null
echo "edge auto-scroll stepped the strip under a held card and the drop landed on the new row"
