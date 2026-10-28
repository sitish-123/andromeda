#!/usr/bin/env bash
# harness: outputs=2
# Overview moves obey destination layout policy in both directions.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[overview]
zoom = 0.5
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg output-focus-next > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
"$ANDROMEDA" msg output-focus-next > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" card 480 300 > "$ANDROMEDA_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$ANDROMEDA" settle > /dev/null
origin=$(title_bar_window card | jq -r '.workspace | split(":")[0]')
destination=$("$ANDROMEDA" outputs --json | jq -r --arg origin "$origin" '.[] | select(.name != $origin) | .name')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Each 1280x720 output has a 640x360 preview centered at +320+180.
drag_to() {
  local dest=$1 output left dest_left x y
  output=$(title_bar_window card | jq -r '.workspace | split(":")[0]')
  left=$("$ANDROMEDA" outputs --json | jq -r --arg name "$output" '.[] | select(.name == $name) | .position.x')
  dest_left=$("$ANDROMEDA" outputs --json | jq -r --arg name "$dest" '.[] | select(.name == $name) | .position.x')
  read -r x y < <(title_bar_window card | jq -r --argjson left "$left" \
    '"\($left + 320 + (.x - $left + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 move "$x" "$y" press 272 \
    move $(((x + dest_left + 640) / 2)) 360 move $((dest_left + 640)) 360 pause 100 release 272
  "$ANDROMEDA" settle > /dev/null
  title_bar_wait_field card workspace "$dest:1"
}
drag_to "$destination"
title_bar_wait_field card floating true
drag_to "$origin"
title_bar_wait_field card floating false
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
echo "overview tiled and floating policy transfers verified"
