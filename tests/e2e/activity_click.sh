#!/usr/bin/env bash
# Clicks the Activity window's button with label TEXT (the Nth match, from 0) through the pointer client. A button
# below the window's bottom edge is scrolled into view first with the wheel over its own column.
# Usage: activity_click.sh TEXT [N]. Needs NOCTALIA, ANDROMEDA and POINTER; positions come from `msg activity-dump`.
set -euo pipefail
target() {
  jq -r --arg t "$1" --argjson n "$2" '[.buttons[]? | select(.text == $t)][$n] // empty | "\(.x | floor) \(.y | floor)"' \
    <<< "$("$NOCTALIA" msg activity-dump)"
}
read -r wx wy wh < <("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "dev.andromeda.Activity") | "\(.x) \(.y) \(.h)"')
pos=$(target "$1" "${2:-0}")
[[ -n $pos ]] || { echo "FAIL: no button \"$1\" in the Activity window" >&2; exit 1; }
read -r bx by <<< "$pos"
if (( by > wh - 24 )); then
  "$POINTER" 1280 720 move $((wx + bx)) $((wy + wh / 2)) axis vertical $((by - wh / 2)) axis-stop vertical > /dev/null
  for _ in $(seq 20); do
    read -r bx by <<< "$(target "$1" "${2:-0}")"
    (( by <= wh - 24 )) && break
    sleep 0.1
  done
fi
"$POINTER" 1280 720 move $((wx + bx)) $((wy + by)) click 272 > /dev/null
