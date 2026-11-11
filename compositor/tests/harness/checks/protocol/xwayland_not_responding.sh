#!/usr/bin/env bash
# harness: xwayland=true
# A close pings an X11 window with _NET_WM_PING: a window that misses the pong is "not responding", one that answers
# but stays open is not. The Xwayland ping timeout is 10 s.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

window_field() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

start_client() {
  local title=$1 command=$2 fifo=$ANDROMEDA_RUNTIME_DIR/$1.fifo log=$ANDROMEDA_RUNTIME_DIR/$1.log
  mkfifo "$fifo"
  "$ANDROMEDA_XWAYLAND_FOCUS_CLIENT" "$title" < "$fifo" > "$log" 2>&1 &
  exec {fd}> "$fifo"
  printf '%s\n' "$command" >&"$fd"
  for _ in $(seq 100); do
    grep -q -- "$command-set" "$log" && [[ -n $(window_field "$title" id) ]] && return 0
    sleep 0.05
  done
  echo "$title never mapped or never took '$command': $(cat "$log")"
  return 1
}

start_client pong-x11 ignore-close
start_client hung-x11 hang
"$ANDROMEDA" msg "window-close:$(window_field pong-x11 id)" > /dev/null
"$ANDROMEDA" msg "window-close:$(window_field hung-x11 id)" > /dev/null

for _ in $(seq 130); do
  [[ $(window_field hung-x11 not_responding) == true ]] && break
  sleep 0.1
done
if [[ $(window_field hung-x11 not_responding) != true ]]; then
  echo "an X11 window that missed the ping is not marked not responding"
  exit 1
fi
# The pong window's ping started first, so its timeout would have fired by now.
if [[ $(window_field pong-x11 not_responding) != false ]]; then
  echo "an X11 window that answered the ping is marked not responding: $(window_field pong-x11 not_responding)"
  exit 1
fi
echo "a missed _NET_WM_PING marks an X11 window not responding; a pong does not"
