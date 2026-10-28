#!/usr/bin/env bash
# harness: outputs=2
# At the workspace limit, another lost output uses a native fallback even when
# the active workspace is already a refuge, preserving its existing tiles.
set -euo pipefail

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
workspaces = 63

[output.HEADLESS-2]
workspaces = 1

[output.HEADLESS-3]
workspaces = 1
EOF
"$ANDROMEDA" msg config-reload > /dev/null

field() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

spawn() {
  local title=$1 selector=$2 count=$3 id
  "$ANDROMEDA" msg "workspace-switch:$selector" > /dev/null
  "$ANDROMEDA_UNMAP_CLIENT" "$title" 800 600 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 50); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') == "$count" ]] && break
    sleep 0.1
  done
  id=$(field "$title" id)
  if [[ -z $id ]]; then
    echo "window '$title' did not map: $(< "$ANDROMEDA_RUNTIME_DIR/$title.log")"
    return 1
  fi
  "$ANDROMEDA" msg "window-focus:$id" > /dev/null
  "$ANDROMEDA" msg "window-move-to-workspace:$selector" > /dev/null
}

spawn local 1/HEADLESS-1 1
readonly LOCAL_HOME="$(field local workspace)"
spawn first-left 1/HEADLESS-2 2
spawn first-right 1/HEADLESS-2 3
"$ANDROMEDA" output-destroy HEADLESS-2 > /dev/null
"$ANDROMEDA" settle
readonly FIRST_REFUGE="$(field first-left workspace)"
workspaces=$("$ANDROMEDA" workspaces --json)
if [[ $FIRST_REFUGE == "$LOCAL_HOME" || $(field first-right workspace) != "$FIRST_REFUGE" ]] || \
    ! jq -e --arg refuge "$FIRST_REFUGE" '
      length == 64 and any(.[]; .id == $refuge and .index == 64 and .named == false and .window_count == 2)
    ' <<< "$workspaces" > /dev/null; then
  summary=$(jq -c '{count:length, occupied:[.[] | select(.occupied)]}' <<< "$workspaces")
  echo "first lost output did not receive a separate temporary workspace at position 64: $("$ANDROMEDA" windows --json) $summary"
  exit 1
fi
"$ANDROMEDA" msg workspace-switch:64/HEADLESS-1 > /dev/null
"$ANDROMEDA" settle
readonly FIRST_LAYOUT="$("$ANDROMEDA" windows --json | jq -c '
  [.[] | select(.title | startswith("first-")) | {title, column, row, x, y, w, h}] | sort_by(.title)
')"
"$ANDROMEDA" output-create HEADLESS-3 > /dev/null
spawn second 1/HEADLESS-3 4
"$ANDROMEDA" msg workspace-switch:64/HEADLESS-1 > /dev/null
"$ANDROMEDA" output-destroy HEADLESS-3 > /dev/null
"$ANDROMEDA" settle
windows=$("$ANDROMEDA" windows --json)
if ! jq -e --arg refuge "$FIRST_REFUGE" '
  length == 4 and all(.[]; .column >= 0) and
  all(.[] | select(.title | startswith("first-")); .workspace == $refuge) and
  any(.[]; .title == "second" and .workspace != $refuge and (.workspace | startswith("HEADLESS-1:")))
' <<< "$windows" > /dev/null; then
  echo "capacity fallback removed a preexisting refugee from its layout: $windows"
  exit 1
fi
actual=$(jq -c '
  [.[] | select(.title | startswith("first-")) | {title, column, row, x, y, w, h}] | sort_by(.title)
' <<< "$windows")
if [[ $actual != "$FIRST_LAYOUT" ]]; then
  echo "capacity fallback changed the occupied refuge layout: expected $FIRST_LAYOUT, got $actual"
  exit 1
fi
workspaces=$("$ANDROMEDA" workspaces --json)
if [[ $(jq 'length' <<< "$workspaces") != 64 ]]; then
  echo "capacity fallback exceeded the workspace limit: $workspaces"
  exit 1
fi
echo "full inventories preserved tiled membership when the active fallback was another temporary workspace"
