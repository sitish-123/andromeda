#!/usr/bin/env bash
# The workspace_slide event streams a keybind switch and a swipe at most once per frame while they slide, ends each
# slide with one idle line at the new workspace, and sends nothing while no slide runs.
set -euo pipefail
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = true

[output."HEADLESS-1"]
workspaces = 3
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null

readonly EVENTS="$ANDROMEDA_RUNTIME_DIR/slide-events.jsonl"
"$ANDROMEDA" subscribe outputs,workspace_slide > "$EVENTS" &
slides() { jq -c 'select(.event == "workspace_slide") | .data' "$EVENTS"; }
count() { slides | jq -s "map(select(.phase == \"$1\")) | length"; }
# Lines leave the compositor before clock-advance answers; this waits for the subscriber to write them.
await_count() {
  for _ in $(seq 100); do
    (($(count "$1") >= $2)) && return 0
    sleep 0.05
  done
  echo "expected $2 $1 lines: $(slides | tr '\n' '|')"
  return 1
}
last() { slides | tail -1; }
for _ in $(seq 100); do
  [[ -s $EVENTS ]] && break
  sleep 0.05
done
swipe() { "$ANDROMEDA" swipe-test "$@" > /dev/null; }

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" clock-advance 100 > /dev/null
[[ -z $(slides) ]] || { echo "idle compositor sent: $(slides)"; exit 1; }

# Keybind switch 1 -> 2: every frame reports the fraction between the two workspaces.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count settling 1
last | jq -e '.output == "HEADLESS-1" and .from != .to and .position > 1 and .position < 2
  and .progress > 0 and .progress < 1' > /dev/null || { echo "first keybind frame: $(last)"; exit 1; }
"$ANDROMEDA" clock-advance 5000 > /dev/null
await_count idle 1
last | jq -e '.phase == "idle" and .position == 2 and .from == .to' > /dev/null || { echo "keybind end: $(last)"; exit 1; }
"$ANDROMEDA" clock-advance 100 > /dev/null
"$ANDROMEDA" clock-advance 100 > /dev/null

# Swipe 2 -> 3: the touchpad events between two frames make one line.
swipe begin 2000
swipe update 2050 0 -60
swipe update 2075 0 -60
swipe update 2100 0 -60
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count tracking 1
last | jq -e '.phase == "tracking" and .position > 2 and .position < 3' > /dev/null || { echo "swipe frame: $(last)"; exit 1; }
swipe update 2150 0 -60
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count tracking 2
swipe end 2600 0
"$ANDROMEDA" clock-advance 5000 > /dev/null
await_count idle 2
last | jq -e '.position == 3' > /dev/null || { echo "swipe end: $(last)"; exit 1; }
"$ANDROMEDA" clock-advance 100 > /dev/null
"$ANDROMEDA" clock-advance 100 > /dev/null

# A last switch marks the end of the idle stretch: a line sent while idle would sit before its first frame.
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" clock-advance 16 > /dev/null
await_count settling 3
last | jq -e '.position > 1 and .position < 3' > /dev/null || { echo "jump frame: $(last)"; exit 1; }
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
await_count idle 3
last | jq -e '.position == 1' > /dev/null || { echo "jump end: $(last)"; exit 1; }

[[ $(count tracking) -eq 2 ]] || { echo "tracking lines: $(count tracking), expected one per frame"; exit 1; }
phases=$(slides | jq -r .phase | uniq | tr '\n' ' ')
expected='settling idle tracking settling idle settling idle '
[[ $phases == "$expected" ]] || { echo "phases: '$phases', expected '$expected'"; exit 1; }
echo 'workspace_slide streams keybind and swipe slides once per frame, ends with idle, and is silent while idle'
