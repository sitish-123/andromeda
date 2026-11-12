#!/usr/bin/env bash
# Reduce motion (rulebook 7.8): the workspace switch crossfades, yet a swipe still tracks the fingers and the
# workspace_slide event still streams its position, so the bar follows. A fast release settles without velocity carry
# or overshoot, and a keybind switch streams the same way.
set -euo pipefail
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = true

[appearance]
reduce_motion = true

[output."HEADLESS-1"]
workspaces = 3
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null

readonly EVENTS="$ANDROMEDA_RUNTIME_DIR/slide-events.jsonl"
"$ANDROMEDA" subscribe outputs,workspace_slide > "$EVENTS" &
for _ in $(seq 100); do
  [[ -s $EVENTS ]] && break
  sleep 0.05
done
slides() { jq -c 'select(.event == "workspace_slide") | .data' "$EVENTS"; }
count() { slides | jq -s --arg phase "$1" 'map(select(.phase == $phase)) | length'; }
await_count() {
  for _ in $(seq 100); do
    (($(count "$1") >= $2)) && return 0
    sleep 0.05
  done
  echo "expected $2 $1 lines: $(slides | tr '\n' '|')"
  exit 1
}
swipe() { "$ANDROMEDA" swipe-test "$@" > /dev/null; }

"$ANDROMEDA" clock-freeze > /dev/null

# A fast swipe 1 -> 2 tracks the fingers, then settles on 2 without passing it.
swipe begin 1000
swipe update 1008 0 -40
swipe update 1016 0 -40
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count tracking 1
slides | tail -1 | jq -e '.phase == "tracking" and .position > 1 and .position < 2' > /dev/null \
  || { echo "tracking frame: $(slides | tail -1)"; exit 1; }
swipe update 1024 0 -40
swipe update 1032 0 -40
swipe end 1040 0
for _ in $(seq 40); do
  "$ANDROMEDA" clock-advance 16 > /dev/null
done
await_count idle 1
slides | tail -1 | jq -e '.position == 2' > /dev/null || { echo "swipe end: $(slides | tail -1)"; exit 1; }
peak=$(slides | jq -s 'map(select(.phase == "settling") | .position) | max')
jq -e -n --argjson peak "$peak" '$peak <= 2.0001' > /dev/null || { echo "settle overshot to $peak"; exit 1; }

# A keybind switch streams its crossfade's progress too.
settled=$(count settling)
"$ANDROMEDA" msg workspace-switch:3 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count settling $((settled + 1))
slides | tail -1 | jq -e '.position > 2 and .position < 3' > /dev/null || { echo "keybind frame: $(slides | tail -1)"; exit 1; }
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
await_count idle 2
echo 'under reduce motion a swipe tracks and streams, settles without overshoot, and a keybind switch streams'
