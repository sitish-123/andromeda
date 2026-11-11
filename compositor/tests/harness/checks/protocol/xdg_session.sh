#!/usr/bin/env bash
# xdg-session-management-v1: a window added to a session and moved to another workspace returns to that workspace
# when the app restores the session after it quits; the compositor keeps the session in its state file.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_XDG_SESSION_CLIENT:-./build-debug/tests/xdg-session-client}"
readonly FILLER="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FILLER_LOG="$ANDROMEDA_RUNTIME_DIR/xdg-session-filler.log"
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/xdg-session-first.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/xdg-session-second.log"
readonly STATE_FILE="$ANDROMEDA_RUNTIME_DIR/xdg-state/andromeda/xdg-sessions.json"

if [[ ! -x $CLIENT || ! -x $FILLER ]]; then
  echo "required harness clients are not built"
  exit 1
fi

# Prints the workspace of the window with app id xdg-session-test, or nothing while none is mapped.
window_workspace() {
  "$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "xdg-session-test") | .workspace'
}

# A window that stays on the first workspace, so the saved second workspace is the trailing one again after the app
# quits (dynamic workspaces drop an empty one).
APP_ID=xdg-session-filler "$FILLER" xdg-session-filler > "$FILLER_LOG" 2>&1 &
filler_pid=$!
trap 'kill "$filler_pid" "${first_pid:-}" "${second_pid:-}" 2> /dev/null || true' EXIT
for _ in $(seq 60); do
  "$ANDROMEDA" windows --json | jq -e 'any(.[]; .app_id == "xdg-session-filler")' > /dev/null && break
  sleep 0.1
done

"$CLIENT" > "$FIRST_LOG" 2>&1 &
first_pid=$!
await_lines "$FIRST_LOG" 'created ' 1
await_lines "$FIRST_LOG" 'mapped' 1
session_id=$(sed -n 's/^created //p' "$FIRST_LOG")

home=$("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .id')
window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "xdg-session-test") | .id')
"$ANDROMEDA" msg "window-focus:$window_id" > /dev/null
"$ANDROMEDA" msg "window-move-to-workspace-silent:2" > /dev/null
for _ in $(seq 60); do
  moved=$(window_workspace)
  [[ -n $moved && $moved != "$home" ]] && break
  sleep 0.1
done
if [[ -z $moved || $moved == "$home" ]]; then
  echo "the window did not move off workspace $home"
  exit 1
fi

kill "$first_pid"
wait "$first_pid" 2> /dev/null || true
for _ in $(seq 60); do
  [[ -z $(window_workspace) ]] && break
  sleep 0.1
done

# The state file holds the position "OUTPUT:2"; workspace ids count creations.
saved=
for _ in $(seq 60); do
  saved=$(jq -r --arg id "$session_id" '.sessions[$id].main.workspace // empty' "$STATE_FILE" 2> /dev/null || true)
  [[ $saved == "HEADLESS-1:2" ]] && break
  sleep 0.1
done
if [[ $saved != "HEADLESS-1:2" ]]; then
  echo "session $session_id does not hold workspace position 2 in $STATE_FILE: $(cat "$STATE_FILE" 2> /dev/null)"
  exit 1
fi

"$CLIENT" "$session_id" > "$SECOND_LOG" 2>&1 &
second_pid=$!
await_lines "$SECOND_LOG" 'session-restored' 1
await_lines "$SECOND_LOG" 'toplevel-restored' 1
await_lines "$SECOND_LOG" 'mapped' 1
for _ in $(seq 60); do
  restored=$(window_workspace)
  [[ -n $restored ]] && break
  sleep 0.1
done
position=$("$ANDROMEDA" workspaces --json | jq -r --arg id "$restored" '.[] | select(.id == $id) | .index')
if [[ $position != 2 ]]; then
  echo "the restored window opened on '$restored' at position '$position', not at its saved position 2"
  exit 1
fi
echo "session $session_id: window saved at $saved (id $moved), restored on $restored at position $position"
