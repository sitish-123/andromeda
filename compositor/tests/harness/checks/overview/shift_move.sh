#!/usr/bin/env bash
# Shift+Right moves the selected window card to the next workspace and Shift+Left brings it back, with the overview
# open (DESIGN.md "Overview v3" Completion).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly K_LEFT=105 K_RIGHT=106

keys() { "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "floating"

[overview]
style = "spaces"
background_blur = false

[animation]
enabled = false
EOF
"$ANDROMEDA" msg config-reload > /dev/null

workspace_of() { "$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "shift-move") | .workspace'; }
wait_workspace() {
  for _ in $(seq 60); do
    [[ $(workspace_of) == "$1" ]] && return 0
    sleep 0.05
  done
  echo "$2: $("$ANDROMEDA" windows --json)"
  return 1
}

# A second window keeps the first workspace: an emptied dynamic workspace is removed.
"$ANDROMEDA_UNMAP_CLIENT" shift-anchor 500 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA_UNMAP_CLIENT" shift-move 500 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ -n $(workspace_of) ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
start=$(workspace_of)
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

keys mod shift tap "$K_RIGHT" mod none
for _ in $(seq 60); do
  [[ $(workspace_of) != "$start" ]] && break
  sleep 0.05
done
moved=$(workspace_of)
[[ $moved != "$start" ]] || { echo "Shift+Right left the window on workspace $start"; exit 1; }
"$ANDROMEDA" settle
keys mod shift tap "$K_LEFT" mod none
wait_workspace "$start" "Shift+Left did not bring the window back from $moved"
echo "Shift+arrows move the selected window between workspaces ($start -> $moved -> $start)"
