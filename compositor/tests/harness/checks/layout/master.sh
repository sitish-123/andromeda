#!/usr/bin/env bash
# Real clients exercise master count changes, directional and cyclic focus order, and swap ordering through compositor seams.
set -euo pipefail

spawn_client() {
  foot --title="harness-master-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for $want window(s), saw: $("$ANDROMEDA" windows --json)"
  return 1
}

wait_for_query() {
  local query=$1
  local message=$2
  for _ in $(seq 40); do
    if "$ANDROMEDA" windows --json | jq -e "$query" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "$message: $("$ANDROMEDA" windows --json)"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "master"

[animation]
duration_ms = 1
curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

# The 1280x720 headless output uses edgePad 10 and totalGap 12. Content is 1260x700. With the default 0.55 master
# fraction, the divisible width is 1248: master round(0.55 * 1248) = 686, stack = 562. Two rows share
# 700 - 12 = 688 pixels, so each is 344 pixels high.
spawn_client a
wait_for_windows 1

spawn_client b
wait_for_windows 2

spawn_client c
wait_for_windows 3

# Focus memory: leaving the stack and coming back lands on the row that was focused last.
"$ANDROMEDA" msg window-focus-left > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "focus-left did not reach master"
"$ANDROMEDA" msg window-focus-right > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .focused == true)' \
  "focus-right did not land on the stack top first"
"$ANDROMEDA" msg window-focus-down > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "focus-down did not reach the lower stack row"
"$ANDROMEDA" msg window-focus-left > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "focus-left did not return to master"
"$ANDROMEDA" msg window-focus-right > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "focus-right did not return to the last-focused stack row"
"$ANDROMEDA" msg window-focus-up > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .focused == true)' \
  "focus-up did not restore c for the consume step"

# consume-left pulls c into the master column so the count actions have two master rows to move.
"$ANDROMEDA" msg window-consume-left > /dev/null

"$ANDROMEDA" msg window-focus-right > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "focus-right did not cross from master to stack"

"$ANDROMEDA" msg window-focus-left > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .focused == true)' \
  "focus-left did not cross back to the master column's last-focused row"

"$ANDROMEDA" msg layout-master-count-decrease > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .x == 10 and .y == 10 and .w == 686 and .h == 700) and any(.[]; .title == "harness-master-c" and .x == 708 and .y == 10 and .w == 562 and .h == 344) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 366 and .w == 562 and .h == 344)' \
  "layout-master-count-decrease did not demote the last master window to the stack top"

"$ANDROMEDA" msg layout-master-count-increase > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .x == 10 and .y == 10 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-c" and .x == 10 and .y == 366 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 10 and .w == 562 and .h == 700)' \
  "layout-master-count-increase did not promote the stack top to the master bottom"

# Cycling below starts from the master top row.
"$ANDROMEDA" msg window-focus-up > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "focus-up did not reach the master top row"

"$ANDROMEDA" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .focused == true)' \
  "window-focus-next did not focus c after a"
"$ANDROMEDA" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "window-focus-next did not focus b after c"
"$ANDROMEDA" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "window-focus-next did not wrap from b to a"
"$ANDROMEDA" msg window-focus-previous > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "window-focus-previous did not wrap from a to b"
"$ANDROMEDA" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "window-focus-next did not return focus from b to a"

"$ANDROMEDA" msg window-swap-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .x == 10 and .y == 10 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-a" and .x == 10 and .y == 366 and .w == 686 and .h == 344 and .focused == true) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 10 and .w == 562 and .h == 700)' \
  "window-swap-next did not exchange master rows while retaining focus"

echo "master count, focus memory, focus order, and swap ordering hold with real clients"
