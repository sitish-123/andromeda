#!/usr/bin/env bash
# Three keybind switches with no frame between the 2nd and 3rd never carry a slide past one step: the workspace on screen
# beside the active one stays within reach of the slide (rulebook 7.1 r5).
set -euo pipefail
cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[animation]
enabled = true

[output."HEADLESS-1"]
workspaces = 4
EOF2
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null

readonly EVENTS="$ANDROMEDA_RUNTIME_DIR/slide-rapid-events.jsonl"
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

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines 2
lines=$(slides | wc -l)
"$ANDROMEDA" msg workspace-switch:3 > /dev/null
"$ANDROMEDA" msg workspace-switch:4 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_lines $((lines + 1))
slides | tail -1 | jq -e '(.progress | fabs) <= 1' > /dev/null || { echo "slide past one step: $(slides | tail -1)"; exit 1; }

"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
echo 'rapid keybind switches keep the slide within one step'
