#!/usr/bin/env bash
# The overview gesture has a second stop: pinching on past the open stop shows every workspace as the Grid, and
# pinching back out from there returns to the configured style without closing.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[overview]
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
pinch() { "$ANDROMEDA" pinch-test "$@" --json; }

pinch begin 1000 4 > /dev/null
pinch update 1100 0.6 > /dev/null
held=$(pinch update 1200 0.4 | jq -c '{active, grid}')
landed=$(pinch end 1300 0 | jq -c '{active, grid}')
"$ANDROMEDA" settle > /dev/null
[[ $held == '{"active":true,"grid":true}' ]] || { echo "halfway to the second stop should show the Grid: $held"; exit 1; }
[[ $landed == '{"active":true,"grid":true}' ]] || { echo "a pinch past the open stop did not land on the Grid: $landed"; exit 1; }

pinch begin 2000 4 > /dev/null
pinch update 2100 1.4 > /dev/null
back=$(pinch end 2200 0 | jq -c '{active, grid}')
"$ANDROMEDA" settle > /dev/null
[[ $back == '{"active":true,"grid":false}' ]] || { echo "pinching out of the Grid should step back to the open stop: $back"; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
echo "the gesture's second stop shows the Grid and steps back to the configured style"
