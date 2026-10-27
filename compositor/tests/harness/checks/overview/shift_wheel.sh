#!/usr/bin/env bash
# Wheel navigation uses physical axes and discrete column/workspace targets, with independent factors.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[overview]
scroll_factor_horizontal = 0.5
scroll_factor_vertical = 2.0

[layout.scrolling]
default_extent_fraction = 0.5

[output."HEADLESS-1"]
workspaces = 3
EOF
"$ANDROMEDA" msg config-reload > /dev/null

for index in 1 2 3; do
  "$ANDROMEDA_UNMAP_CLIENT" "shift-wheel-$index" 1200 700 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq length) == "$index" ]] && break
    sleep 0.05
  done
done
[[ $("$ANDROMEDA" windows --json | jq length) == 3 ]]
"$ANDROMEDA" msg column-focus-first > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null

first_x() {
  "$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "shift-wheel-1") | .x'
}
selected_title() {
  "$ANDROMEDA" windows --json | jq -r '.[] | select(.focused) | .title'
}
active_workspace() {
  "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .index'
}
before=$(first_x)
# Keep one virtual keyboard/pointer alive for the two half-factor notches.
pointer_hold 1280 720 move 640 360 mod shift notch 1 -- notch 1 mod none
[[ $(first_x) == "$before" ]] || { echo 'half-factor wheel moved on its first notch'; exit 1; }
[[ $(selected_title) == shift-wheel-1 ]] || { echo "half-factor first notch: $(selected_title)"; exit 1; }
pointer_release
for _ in $(seq 60); do
  [[ $(selected_title) == shift-wheel-2 ]] && break
  sleep 0.05
done
[[ $(selected_title) == shift-wheel-2 ]] || { echo 'Shift-wheel did not select the next column'; exit 1; }
[[ $(active_workspace) == 1 ]] || { echo 'horizontal wheel moved vertically between workspaces'; exit 1; }
(( $(first_x) < before )) || { echo 'Shift-wheel did not reveal the selected column'; exit 1; }
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch -1 notch -1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'reverse Shift-wheel did not select the previous column'; exit 1; }

# A partial notch belongs only to the current overview interaction.
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'half-factor wheel moved before overview close'; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'partial wheel accumulation survived overview close'; exit 1; }
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-2 ]] || { echo 'fresh half-factor notches did not select the next column'; exit 1; }

"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 notch 1
for _ in $(seq 60); do
  [[ $(active_workspace) == 3 ]] && break
  sleep 0.05
done
[[ $(active_workspace) == 3 ]] || { echo 'double vertical factor did not advance two workspaces'; exit 1; }

# The same horizontal input follows the filmstrip when the workspace axis changes.
printf '\nworkspace_axis = "horizontal"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
# In this arrangement the vertical wheel steps columns, not workspaces. Factor 2 skips to column 3.
"$ANDROMEDA" msg column-focus-first > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 notch 1
[[ $(selected_title) == shift-wheel-3 ]] || { echo 'vertical wheel did not step vertical columns'; exit 1; }
[[ $(active_workspace) == 1 ]] || { echo 'vertical wheel switched horizontal workspaces'; exit 1; }
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch 1 notch 1 mod none
[[ $(active_workspace) == 2 ]] || { echo 'Shift-wheel did not follow horizontal workspaces'; exit 1; }

# A user binding wins, even though the default horizontal factor would require two notches.
printf '\n[keybinds]\n"Shift+WheelDown" = "workspace-next"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(active_workspace) == 3 ]] || { echo 'Shift-wheel ignored the configured binding'; exit 1; }
echo 'wheel steps vertically, Shift-wheel steps horizontally, factors count notches, and bindings win'
