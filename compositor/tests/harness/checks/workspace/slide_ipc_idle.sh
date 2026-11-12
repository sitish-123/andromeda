#!/usr/bin/env bash
# harness: outputs=2
# Every way a workspace slide can end sends the workspace_slide idle line for its output: a cancelled swipe, the
# overview opening mid-slide, a config reload that removes workspaces mid-slide, and the output leaving mid-slide.
# A shell that follows the slide drops its follow state on that line.
set -euo pipefail
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = true

[output."HEADLESS-1"]
workspaces = 3

[output."HEADLESS-2"]
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
slides() { jq -c --arg output "$1" 'select(.event == "workspace_slide" and .data.output == $output) | .data' "$EVENTS"; }
count() { slides "$1" | jq -s --arg phase "$2" 'map(select(.phase == $phase)) | length'; }
# Lines leave the compositor before clock-advance answers; this waits for the subscriber to write them.
await_count() {
  for _ in $(seq 100); do
    (($(count "$1" "$2") >= $3)) && return 0
    sleep 0.05
  done
  echo "$4: expected $3 $2 lines on $1: $(slides "$1" | tr '\n' '|')"
  exit 1
}
swipe() { "$ANDROMEDA" swipe-test "$@" > /dev/null; }

"$ANDROMEDA" clock-freeze > /dev/null

# A cancelled swipe settles back and ends on the workspace it started from (swipes drive the focused HEADLESS-2).
swipe begin 1000
swipe update 1050 0 -60
swipe update 1100 0 -60
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-2 tracking 1 'swipe'
swipe end 1150 1
"$ANDROMEDA" clock-advance 5000 > /dev/null
await_count HEADLESS-2 idle 1 'cancelled swipe'
slides HEADLESS-2 | tail -1 | jq -e '.position == 1' > /dev/null || { echo "cancel end: $(slides HEADLESS-2 | tail -1)"; exit 1; }

# The overview opening mid-slide ends the slide.
"$ANDROMEDA" msg workspace-switch:2/HEADLESS-1 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-1 settling 1 'switch before overview'
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-1 idle 1 'overview open'
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" clock-advance 5000 > /dev/null

# A reload that removes the workspace the slide heads for ends the slide.
settled=$(count HEADLESS-1 settling)
"$ANDROMEDA" msg workspace-switch:3/HEADLESS-1 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-1 settling $((settled + 1)) 'switch before removal'
sed -i 's/^workspaces = 3$/workspaces = 1/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-1 idle 2 'workspace removal'

# An output unplugged mid-slide still ends its slide.
sed -i 's/^workspaces = 1$/workspaces = 3/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
settled=$(count HEADLESS-2 settling)
"$ANDROMEDA" msg workspace-switch:2/HEADLESS-2 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count HEADLESS-2 settling $((settled + 1)) 'switch before unplug'
"$ANDROMEDA" output-destroy HEADLESS-2 > /dev/null
await_count HEADLESS-2 idle 2 'unplug'

"$ANDROMEDA" clock-resume > /dev/null
echo 'a cancelled swipe, the overview, a workspace removal and an unplug each end the slide with an idle line'
