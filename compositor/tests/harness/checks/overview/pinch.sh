#!/usr/bin/env bash
# A 4-finger pinch in opens the overview and tracks the fingers; a short pinch that is cancelled leaves it closed
# (docs/gestures-minimize.md G3).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
pinch() { "$ANDROMEDA" pinch-test "$@" --json; }

pinch begin 1000 4 > /dev/null
half=$(pinch update 1100 0.8 | jq -r '.progress')
pinch update 1200 0.6 > /dev/null
opened=$(pinch end 1300 0 | jq -r '.active')
"$ANDROMEDA" settle > /dev/null
awk -v p="$half" 'BEGIN { exit !(p > 0.3 && p < 0.7) }' ||
  { echo "pinching in to 0.8 should track halfway open, got progress $half"; exit 1; }
[[ $opened == true ]] || { echo "a full 4-finger pinch in did not open the overview"; exit 1; }

"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
pinch begin 2000 4 > /dev/null
pinch update 2100 0.9 > /dev/null
pinch end 2200 1 > /dev/null
"$ANDROMEDA" settle > /dev/null
state=$(pinch update 2300 1 | jq -r '.active')
[[ $state == false ]] || { echo "a cancelled pinch left the overview open"; exit 1; }

pinch begin 3000 2 > /dev/null
pinch update 3100 0.5 > /dev/null
two=$(pinch end 3200 0 | jq -r '.active')
[[ $two == false ]] || { echo "a 2-finger pinch opened the overview; it belongs to the app"; exit 1; }
echo "a 4-finger pinch tracks and opens the overview; cancelled and 2-finger pinches do not"
