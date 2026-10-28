#!/usr/bin/env bash
# center_on_overview_select centers the column picked from the overview even when it already fits, which center_focused =
# "never" would leave in place. The first column rests at the left edge, so only the option can move it.
set -euo pipefail

readonly KEY_ENTER=28
BASELINE=$(< "$ANDROMEDA_CONFIG")

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation]\nenabled = false\n'
    printf '\n[appearance]\nborder_width = 0\nouter_border_width = 0\n'
    printf '\n[layout]\ngap = 0\n'
    printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\ncenter_focused = "never"\n'
    printf 'center_on_overview_select = %s\n' "$1"
  } > "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
}

first_x() {
  "$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "center-select-1") | .x'
}

select_first_from_overview() {
  local id
  id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "center-select-1") | .id')
  "$ANDROMEDA" msg "window-focus:$id" > /dev/null
  "$ANDROMEDA" msg column-focus-first > /dev/null
  "$ANDROMEDA" settle
  [[ $(first_x) == 0 ]] || { echo "first column did not rest at the left edge: x=$(first_x)"; exit 1; }
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle
  "$ANDROMEDA_POINTER_CLIENT" 1280 720 tap "$KEY_ENTER"
  "$ANDROMEDA" settle
}

write_config false
for index in 1 2 3; do
  "$ANDROMEDA_UNMAP_CLIENT" "center-select-$index" 1200 700 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq length) == "$index" ]] && break
    sleep 0.05
  done
done
[[ $("$ANDROMEDA" windows --json | jq length) == 3 ]]

select_first_from_overview
[[ $(first_x) == 0 ]] || { echo "selection without center_on_overview_select moved a fitting column: x=$(first_x)"; exit 1; }

write_config true
select_first_from_overview
# A 640px column on a 1280px output centers at x=320.
[[ $(first_x) == 320 ]] || { echo "center_on_overview_select did not center the selected column: x=$(first_x)"; exit 1; }
echo 'overview selection centers the chosen column only with center_on_overview_select'
