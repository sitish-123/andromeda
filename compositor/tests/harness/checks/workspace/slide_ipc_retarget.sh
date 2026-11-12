#!/usr/bin/env bash
# A keybind switch while a swipe still settles starts from where the slide is on screen (rulebook 7.1 r5): its first
# workspace_slide line sits next to the last one, with no snap to the settled workspace and no restart from it.
set -euo pipefail
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = true

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
await_lines() {
  for _ in $(seq 100); do
    (($(slides | wc -l) >= $1)) && return 0
    sleep 0.05
  done
  echo "expected $1 lines: $(slides | tr '\n' '|')"
  exit 1
}
swipe() { "$ANDROMEDA" swipe-test "$@" > /dev/null; }

"$ANDROMEDA" clock-freeze > /dev/null

# Swipe 1 -> 2 and release; two frames into the settle the slide sits between 1 and 2.
swipe begin 1000
swipe update 1050 0 -60
swipe update 1100 0 -60
swipe update 1150 0 -60
"$ANDROMEDA" clock-advance 16 > /dev/null
swipe end 1600 0
"$ANDROMEDA" clock-advance 16 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines 3
before=$(slides | tail -1)
jq -e '.phase == "settling" and .position > 1 and .position < 1.8' <<< "$before" > /dev/null \
  || { echo "settle frame: $before"; exit 1; }
lines=$(slides | wc -l)

# Back to 1 by keybind: the next frame moves on from where the settle was.
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines $((lines + 1))
after=$(slides | tail -1)
jq -e -n --argjson before "$before" --argjson after "$after" \
  '$after.phase == "settling" and ($after.position - $before.position | fabs) < 0.15' \
  > /dev/null || { echo "retarget: $before -> $after"; exit 1; }
idles=$(slides | jq -s 'map(select(.phase == "idle")) | length')
[[ $idles -eq 0 ]] || { echo "the keybind snapped the settle to its end: $(slides | tr '\n' '|')"; exit 1; }

# On to 3 by keybind while that one settles: workspace 1 stays on screen beside 2 and slides out.
lines=$(slides | wc -l)
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines $((lines + 1))
lines=$(slides | wc -l)
before=$(slides | tail -1)
"$ANDROMEDA" msg workspace-switch:3 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines $((lines + 1))
after=$(slides | tail -1)
jq -e -n --argjson before "$before" --argjson after "$after" \
  '$before.position < 1.8 and ($after.position - $before.position | fabs) < 0.15' > /dev/null \
  || { echo "carry across: $before -> $after"; exit 1; }

"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
for _ in $(seq 100); do
  [[ $(slides | tail -1 | jq -r .phase) == idle ]] && break
  sleep 0.05
done
slides | tail -1 | jq -e '.phase == "idle" and .position == 3' > /dev/null || { echo "end: $(slides | tail -1)"; exit 1; }
echo 'a keybind switch during a settle carries on from the slide position on screen'
