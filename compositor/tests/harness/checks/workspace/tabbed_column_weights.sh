#!/usr/bin/env bash
# Exercise the private rebuildTransferredColumn path with real client configures.
# Unit coverage pins master's exported sum; this check verifies the transfer
# restores the shared weight through a tab stepping out and a new row joining.
set -euo pipefail

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq length) == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected $1 windows"
  return 1
}

"$ANDROMEDA" msg workspace-next
"$ANDROMEDA" msg workspace-set-layout:scrolling
"$ANDROMEDA" msg workspace-previous
"$ANDROMEDA" msg workspace-set-layout:master
foot --title=group-weight-a sh -c 'sleep 120' > "$ANDROMEDA_RUNTIME_DIR/a.log" 2>&1 &
wait_for_windows 1
foot --title=group-weight-b sh -c 'sleep 120' > "$ANDROMEDA_RUNTIME_DIR/b.log" 2>&1 &
wait_for_windows 2
"$ANDROMEDA" msg layout-master-count-increase
"$ANDROMEDA" settle
read -r first last <<< "$("$ANDROMEDA" windows --json | jq -r 'sort_by(.y) | "\(.[0].id) \(.[1].id)"')"
"$ANDROMEDA" msg "window-focus:$first"
"$ANDROMEDA" msg window-set-secondary-extent:0.25
"$ANDROMEDA" settle
source=$("$ANDROMEDA" windows --json)
if ! jq -e --arg first "$first" --arg last "$last" '
  (.[] | select(.id == $first) | .h) < (.[] | select(.id == $last) | .h)
' <<< "$source" > /dev/null; then
  echo "master source rows did not acquire unequal proportions: $source"
  exit 1
fi
"$ANDROMEDA" msg column-set-display:tabbed
"$ANDROMEDA" msg column-move-to-workspace-next
"$ANDROMEDA" settle
"$ANDROMEDA" msg "window-focus:$last"
"$ANDROMEDA" msg window-move-down
"$ANDROMEDA" settle
foot --title=group-weight-incoming sh -c 'sleep 120' > "$ANDROMEDA_RUNTIME_DIR/incoming.log" 2>&1 &
wait_for_windows 3
"$ANDROMEDA" msg window-consume-left
"$ANDROMEDA" settle
windows=$("$ANDROMEDA" windows --json)
if ! jq -e --arg first "$first" --arg last "$last" '
  length == 3
  and all(.[]; (.tab_hidden | not))
  and (map(.x) | unique | length) == 1
  and any(.[]; .id == $first and .tabbed)
  and any(.[]; .id == $last and (.tabbed | not))
  and any(.[]; .title == "group-weight-incoming" and (.tabbed | not))
' <<< "$windows" > /dev/null; then
  echo "transferred group and standalone rows did not share one column: $windows"
  exit 1
fi
last_height=$(jq -r --arg id "$last" '.[] | select(.id == $id) | .h' <<< "$windows")
incoming_height=$(jq -r '.[] | select(.title == "group-weight-incoming") | .h' <<< "$windows")
# The source weights become 1/3 and 1 after the 0.25 resize. Its tabbed
# unit exports their sum, 4/3; the independent incoming row has weight 1.
# Each configured height can round by a pixel, so the scaled difference
# has a tolerance of 3 + 4 pixels.
scaled_difference=$((3 * last_height - 4 * incoming_height))
if ((scaled_difference < -7 || scaled_difference > 7)); then
  echo "transferred master tabs lost their total row weight: incoming=$incoming_height stepped-out=$last_height"
  echo "$windows"
  exit 1
fi
echo "transferred master tabs retained the scrolling unit weight after a tab stepped out"
