#!/usr/bin/env bash
# Overview keys: typing (Shift included) filters the cards by title and Enter opens the match; Esc clears the filter
# before it closes; Tab steps the selection; Delete closes the selected window; a digit jumps to that workspace. None of
# these keys reach a client.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG_DIR="$ANDROMEDA_RUNTIME_DIR"

# evdev key codes
readonly K_ESC=1 K_2=3 K_BACKSPACE=14 K_TAB=15 K_ENTER=28 K_DELETE=111
readonly K_A=30 K_B=48 K_E=18 K_T=20 K_G=34 K_M=50 K_Z=44

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "floating"

[overview]
style = "spaces"
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

finish() {
  "$ANDROMEDA" clock-advance 2000
}

window_count() {
  "$ANDROMEDA" windows --json | jq 'length'
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(window_count) -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$ANDROMEDA" windows --json)"
  return 1
}

focused_title() {
  "$ANDROMEDA" windows --json | jq -r '[.[] | select(.focused) | .title] | if length == 1 then .[0] else "none" end'
}

focus_title() {
  local id
  id=$("$ANDROMEDA" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id')
  "$ANDROMEDA" msg "window-focus:$id" > /dev/null
}

active_workspace() {
  "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .name'
}

open_overview() {
  "$ANDROMEDA" msg overview-open > /dev/null
  finish
}

keys() {
  pointer "$@"
  finish
}

"$ANDROMEDA" clock-freeze
count=0
for name in alpha beta gamma; do
  "$CLIENT" "filter-$name" 600 400 > "$LOG_DIR/filter-$name.log" 2>&1 &
  count=$((count + 1))
  wait_for_count $count
done
focus_title filter-alpha
finish

# Capitals reach the filter; Enter opens the one match.
open_overview
keys mod shift tap $K_B tap $K_E tap $K_T mod none tap $K_A tap $K_ENTER
if [[ $(focused_title) != filter-beta ]]; then
  echo "typing 'BETa' + Enter did not open filter-beta: $("$ANDROMEDA" windows --json)"
  exit 1
fi

# Esc with a query only clears it: the overview stays up, so Tab moves the selection off beta and Enter opens it.
open_overview
keys tap $K_Z tap $K_Z tap $K_ESC tap $K_TAB tap $K_ENTER
after_tab=$(focused_title)
if [[ $after_tab == filter-beta || $after_tab == none ]]; then
  echo "Esc closed the overview or Tab did not move the selection (focused: $after_tab)"
  exit 1
fi

# Backspace edits the query; Delete closes the selected match and leaves the others.
open_overview
keys tap $K_G tap $K_A tap $K_M tap $K_Z tap $K_BACKSPACE tap $K_DELETE
wait_for_count 2
if "$ANDROMEDA" windows --json | jq -e '.[] | select(.title == "filter-gamma")' > /dev/null; then
  echo "Delete did not close filter-gamma: $("$ANDROMEDA" windows --json)"
  exit 1
fi
keys tap $K_ESC

# A digit jumps to that workspace without leaving the overview; Esc then closes onto it.
before=$(active_workspace)
open_overview
keys tap $K_2
if [[ $(active_workspace) == "$before" ]]; then
  echo "digit 2 did not switch workspace (still $before): $("$ANDROMEDA" workspaces --json)"
  exit 1
fi
keys tap $K_ESC

for name in alpha beta gamma; do
  if grep -qE '^key ([0-9]+) 1$' "$LOG_DIR/filter-$name.log" 2> /dev/null; then
    echo "an overview key reached filter-$name:"
    grep -E '^key ' "$LOG_DIR/filter-$name.log"
    exit 1
  fi
done
echo "ok"
