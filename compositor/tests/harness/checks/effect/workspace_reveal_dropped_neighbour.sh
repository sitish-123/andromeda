#!/usr/bin/env bash
# A swipe that settles away from one neighbour must take the reveal binding off it.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
workspace_reveal_config
"$ANDROMEDA" msg config-reload > /dev/null
for n in 1 2 3; do
  "$ANDROMEDA" msg "workspace-switch:$n" > /dev/null
  "$ANDROMEDA" settle
  case $n in 1) color=0xFFFF0000;; 2) color=0xFF00FF00;; 3) color=0xFF0000FF;; esac
  FILL_COLOR=$color "$ANDROMEDA_UNMAP_CLIENT" "dropped-$n" 1280 720 > "$ANDROMEDA_RUNTIME_DIR/$n.log" 2>&1 &
  await_lines "$ANDROMEDA_RUNTIME_DIR/$n.log" mapped 1
  "$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
  "$ANDROMEDA" settle
done
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA_POINTER_CLIENT" 1280 720 move 640 360
"$ANDROMEDA" clock-freeze
swipe() { "$ANDROMEDA" swipe-test "$@" --json; }
swipe begin 1000 > /dev/null
# Show workspace 1 first so the reveal binds it, then release toward workspace 3.
swipe update 1100 0 150 | jq -e '.reveal and (.progress == -0.5)' > /dev/null
"$ANDROMEDA" clock-advance 1
swipe update 1200 0 -390 > /dev/null
"$ANDROMEDA" clock-advance 1
settle=$(swipe end 1700 0)
if ! jq -e '.reveal and .revealBoundOutside == 0' <<< "$settle" > /dev/null; then
  echo "the settle toward workspace 3 left workspace 1 bound to the reveal: $settle"
  exit 1
fi
"$ANDROMEDA" clock-advance 2000
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
"$ANDROMEDA" windows --json | jq -e '.[] | select(.active and .title == "dropped-3")' > /dev/null
