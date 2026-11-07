#!/usr/bin/env bash
# A light 4-finger touch (a few px of jitter, fast) never opens or closes the overview; a real swipe does
# (.agent/DESIGN.md "Overview v3" Gesture: 12 px dead zone, flick needs 24 px of travel).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
swipe() { "$ANDROMEDA" swipe-test "$@" --json; }
active() { "$ANDROMEDA" swipe-test update 0 0 0 --json | jq -r '.overview'; }

swipe begin 1000 4 > /dev/null
during=$(swipe update 1004 0 -3 | jq -r '.overview')
swipe update 1008 0 -3 > /dev/null
swipe end 1010 0 > /dev/null
"$ANDROMEDA" settle > /dev/null
[[ $during == false ]] || { echo "3 px of 4-finger travel already started the overview"; exit 1; }
[[ $(active) == false ]] || { echo "a 6 px 4-finger touch at ~600 px/s opened the overview"; exit 1; }

swipe begin 2000 4 > /dev/null
for t in 2016 2032 2048 2064 2080; do swipe update "$t" 0 -40 > /dev/null; done
swipe end 2096 0 > /dev/null
"$ANDROMEDA" settle > /dev/null
[[ $(active) == true ]] || { echo "a 200 px 4-finger swipe up did not open the overview"; exit 1; }

swipe begin 3000 4 > /dev/null
swipe update 3004 0 3 > /dev/null
swipe update 3008 0 3 > /dev/null
swipe end 3010 0 > /dev/null
"$ANDROMEDA" settle > /dev/null
[[ $(active) == true ]] || { echo "a 6 px 4-finger touch at ~600 px/s closed the open overview"; exit 1; }
echo "a light 4-finger touch leaves the overview as it was; a real swipe opens it"
