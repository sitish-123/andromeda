#!/usr/bin/env bash
# Dwindle column movement follows screen direction rather than depth-first leaf order. In A | (B above C), moving C
# left must swap it with A, not move it upward into B's tile. Leaving and re-entering the right-hand split also
# returns focus to the tile that was focused there last, not to the geometrically nearest one.
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

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$ANDROMEDA" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$ANDROMEDA" windows --json)"
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

spawn_client dwindle-move-left
wait_for_count 1
spawn_client dwindle-move-upper-right
wait_for_count 2
spawn_client dwindle-move-lower-right
wait_for_count 3
sleep 0.1

windows=$("$ANDROMEDA" windows --json)
left_x=$(jq -r '.[] | select(.title == "dwindle-move-left") | .x' <<< "$windows")
upper_x=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .x' <<< "$windows")
upper_y=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .y' <<< "$windows")
lower_id=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .id' <<< "$windows")
lower_x=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .x' <<< "$windows")
lower_y=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .y' <<< "$windows")
if [[ -z $lower_id || $left_x -ge $lower_x || $upper_x -ne $lower_x || $upper_y -ge $lower_y ]]; then
  echo "expected one left tile and two vertically stacked right tiles: $windows"
  exit 1
fi

"$ANDROMEDA" msg "window-focus:$lower_id" > /dev/null
wait_for_focus "$lower_id"

upper_id=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .id' <<< "$windows")
left_id=$(jq -r '.[] | select(.title == "dwindle-move-left") | .id' <<< "$windows")
"$ANDROMEDA" msg window-focus-left > /dev/null
wait_for_focus "$left_id"
"$ANDROMEDA" msg window-focus-right > /dev/null
if ! wait_for_focus "$lower_id"; then
  echo "focus-right did not return to the last-focused tile of the right split (upper is $upper_id)"
  exit 1
fi
"$ANDROMEDA" msg column-move-left > /dev/null

moved_x=$lower_x
for _ in $(seq 40); do
  moved_x=$("$ANDROMEDA" windows --json | jq -r --arg id "$lower_id" '.[] | select(.id == $id) | .x')
  [[ $moved_x -lt $lower_x ]] && break
  sleep 0.1
done
if [[ $moved_x -ne $left_x ]]; then
  echo "expected lower-right window to move left from x=$lower_x to x=$left_x, got: $("$ANDROMEDA" windows --json)"
  exit 1
fi

echo "Dwindle column movement follows horizontal screen geometry"
