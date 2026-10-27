#!/usr/bin/env bash
# Dwindle vertical movement can cross a nested branch. With one upper-right leaf above a three-leaf subtree, a window
# in the lower subtree can move into the upper tile and back down.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$ANDROMEDA" windows --json)"
  return 1
}

wait_for_y() {
  local id=$1
  local want=$2
  local current=
  for _ in $(seq 40); do
    current=$("$ANDROMEDA" windows --json | jq -r --arg id "$id" '.[] | select(.id == $id) | .y')
    [[ $current == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $id at y=$want, got: $("$ANDROMEDA" windows --json)"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
duration_ms = 1
curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client dwindle-nested-left
wait_for_count 1
spawn_client dwindle-nested-upper-right
wait_for_count 2
spawn_client dwindle-nested-lower-left
wait_for_count 3
spawn_client dwindle-nested-lower-right-upper
wait_for_count 4
spawn_client dwindle-nested-lower-right-lower
wait_for_count 5
sleep 0.1

windows=$("$ANDROMEDA" windows --json)
upper_x=$(jq -r '.[] | select(.title == "dwindle-nested-upper-right") | .x' <<< "$windows")
upper_y=$(jq -r '.[] | select(.title == "dwindle-nested-upper-right") | .y' <<< "$windows")
lower_id=$(jq -r '.[] | select(.title == "dwindle-nested-lower-left") | .id' <<< "$windows")
lower_x=$(jq -r '.[] | select(.title == "dwindle-nested-lower-left") | .x' <<< "$windows")
lower_y=$(jq -r '.[] | select(.title == "dwindle-nested-lower-left") | .y' <<< "$windows")
if [[ -z $lower_id || $upper_x -ne $lower_x || $upper_y -ge $lower_y ]]; then
  echo "expected an upper-right leaf above the nested lower subtree: $windows"
  exit 1
fi

"$ANDROMEDA" msg "window-focus:$lower_id" > /dev/null
"$ANDROMEDA" msg window-move-up > /dev/null
wait_for_y "$lower_id" "$upper_y"

"$ANDROMEDA" msg window-move-down > /dev/null
wait_for_y "$lower_id" "$lower_y"

echo "Dwindle vertical movement crosses nested branches in both directions"
