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

# Rulebook 7.7: a pinch let go at over 300 px/s toward open opens, though its projection ends short of half, once it
# travelled 24 px (0.032 of scale); a 12 px flick is a light touch and does nothing (DESIGN.md "Overview v3").
pinch begin 4000 4 > /dev/null
pinch update 4010 0.995 > /dev/null
pinch update 4020 0.99 > /dev/null
pinch update 4030 0.984 > /dev/null
pinch end 4030 0 > /dev/null
"$ANDROMEDA" settle > /dev/null
touch=$(pinch update 4100 1 | jq -r '.active')
[[ $touch == false ]] || { echo "a 12 px pinch flick opened the overview"; exit 1; }
pinch begin 5000 4 > /dev/null
pinch update 5010 0.99 > /dev/null
pinch update 5020 0.975 > /dev/null
pinch update 5030 0.96 > /dev/null
pinch end 5030 0 > /dev/null
"$ANDROMEDA" settle > /dev/null
flick=$(pinch update 5100 1 | jq -r '.active')
[[ $flick == true ]] || { echo "a 30 px pinch let go at ~1000 px/s did not open the overview"; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null

pinch begin 3000 2 > /dev/null
pinch update 3100 0.5 > /dev/null
two=$(pinch end 3200 0 | jq -r '.active')
[[ $two == false ]] || { echo "a 2-finger pinch opened the overview; it belongs to the app"; exit 1; }
echo "a 4-finger pinch tracks and opens the overview; cancelled and 2-finger pinches do not"
