#!/usr/bin/env bash
# harness: outputs=2
# Real relative/absolute input, keyboard escape, reload, client locks and grabs.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/confine.log"
readonly OTHER_LOG="$ANDROMEDA_RUNTIME_DIR/other.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]

[[window_rule]]
default_output = "HEADLESS-1"

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^confined$"
confine_pointer = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
mkfifo "$ANDROMEDA_RUNTIME_DIR/confine-control"
exec 8<> "$ANDROMEDA_RUNTIME_DIR/confine-control"
POINTER_CONSTRAINT_CONTROL=1 "$OBSERVER" confined <&8 > "$LOG" 2>&1 &
game_pid=$!
wait_windows() {
  for _ in $(seq 100); do
    [[ $("$ANDROMEDA" windows --json | jq length) == "$1" ]] && return
    sleep 0.025
  done
  echo "windows did not reach $1"; exit 1
}
wait_windows 1
"$OBSERVER" other > "$OTHER_LOG" 2>&1 &
wait_windows 2
"$ANDROMEDA" settle
windows=$("$ANDROMEDA" windows --json)
game=$(jq -r '.[] | select(.title == "confined") | .id' <<< "$windows")
other=$(jq -r '.[] | select(.title == "other") | .id' <<< "$windows")
read -r gx gy gw gh < <(jq -r '.[] | select(.title == "confined") | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows")
read -r ox oy < <(jq -r '.[] | select(.title == "other") | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"' <<< "$windows")
cx=$((gx + gw / 2)); cy=$((gy + gh / 2))
pointer_hold 2560 720 move "$cx" "$cy" pause 300
"$ANDROMEDA" msg "window-focus:$game" > /dev/null

control() {
  local command=$1 n
  n=$(events "$LOG" "constraint-command $command")
  printf '%s' "$command" >&8
  await_events "$LOG" "constraint-command $command" "$((n + 1))"
}
position() {
  local n
  n=$(events "$LOG" 'pointer-position')
  printf p >&8
  await_events "$LOG" 'pointer-position' "$((n + 1))"
  read -r px py < <(sed -n 's/pointer-position x=\([-0-9]*\) y=\([-0-9]*\)/\1 \2/p' "$LOG" | tail -1)
}
assert_position() {
  position
  if ((px < $1 || px > $2 || py < $3 || py > $4)); then
    echo "pointer $px,$py outside expected $1..$2,$3..$4"; exit 1
  fi
}
move() { "$POINTER" 2560 720 "$@"; }
assert_focus() {
  "$ANDROMEDA" windows --json | jq -e --arg id "$1" '.[] | select(.id == $id) | .active' > /dev/null
}

# Each edge must stop inside the client, with motion along the edge still possible.
move move "$cx" "$cy" relative -2000 0
assert_position 0 1 0 "$gh"
move move "$cx" "$cy" relative 2000 0
assert_position "$((gw - 1))" "$gw" 0 "$gh"
move move "$cx" "$cy" relative 0 -2000
assert_position 0 "$gw" 0 1
move move "$cx" "$cy" relative 0 2000
assert_position 0 "$gw" "$((gh - 1))" "$gh"
move move "$cx" "$cy" move 1279 "$cy"
assert_position "$((gw - 1))" "$gw" 0 "$gh"
move move "$cx" "$cy" move 0 "$cy"
assert_position 0 1 0 "$gh"
move move "$cx" "$cy" move "$cx" 0
assert_position 0 "$gw" 0 1
move move "$cx" "$cy" move "$cx" 719
assert_position 0 "$gw" "$((gh - 1))" "$gh"

# A keyboard focus change with the pointer still over the game must remain an escape.
move move "$cx" "$cy"
"$ANDROMEDA" msg "window-focus:$other" > /dev/null
move relative 1 0
assert_focus "$other"
move move "$ox" "$oy"
assert_focus "$other"
move move "$cx" "$cy"
assert_focus "$game"
move relative 2000 0
assert_position "$((gw - 1))" "$gw" 0 "$gh"

# Explicit warps bypass confinement.
"$ANDROMEDA" msg "window-focus-warp:$other" > /dev/null
assert_focus "$other"
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
assert_focus "$game"

# Preserve client locks and relative motion, then resume confinement in menus.
move move "$cx" "$cy"
control l
await_events "$LOG" pointer-locked 1
relative_events=$(events "$LOG" 'relative-motion x=2000 y=0')
move relative 2000 0
await_events "$LOG" 'relative-motion x=2000 y=0' "$((relative_events + 1))"
assert_position "$((gw / 2 - 1))" "$((gw / 2 + 1))" "$((gh / 2 - 1))" "$((gh / 2 + 1))"
control u
move relative 2000 0
assert_position "$((gw - 1))" "$gw" 0 "$gh"
# A client confinement region narrower than the rule must still win.
move move "$((gx + gw / 4))" "$cy"
control c
await_events "$LOG" pointer-confined 1
move relative 2000 0
assert_position "$((gw / 2 - 1))" "$((gw / 2))" 0 "$gh"
control u
# Keyboard escape also releases a gameplay lock, even on the same workspace.
move move "$cx" "$cy"
control l
"$ANDROMEDA" msg "window-focus:$other" > /dev/null
move relative 1 0
assert_focus "$other"
await_events "$LOG" pointer-unlocked 1
move move "$ox" "$oy"
control u

# Reload must retire keyboard escape while the pointer stays inside the game.
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
"$ANDROMEDA" msg "window-focus:$other" > /dev/null
move relative 1 0
assert_focus "$other"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[[window_rule]]
match.title = "^confined$"
confine_pointer = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
# A native lock may activate independently of keyboard focus once the rule is disabled.
locked=$(events "$LOG" pointer-locked)
control l
await_events "$LOG" pointer-locked "$((locked + 1))"
assert_focus "$other"
control u
# Ordinary keyboard focus invalidation must now allow hover refocus without re-entry.
"$ANDROMEDA" msg "window-focus:$game" > /dev/null
"$ANDROMEDA" msg "window-focus:$other" > /dev/null
move relative 1 0
assert_focus "$game"
move move "$ox" "$oy"
assert_focus "$other"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[[window_rule]]
match.title = "^confined$"
match.is_focused = true
confine_pointer = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
move relative 2000 0
assert_position "$((gw - 1))" "$gw" 0 "$gh"

# Focus-sensitive rules must not lose the escape decision when they stop matching, including a gameplay lock.
move move "$cx" "$cy"
control l
"$ANDROMEDA" msg "window-focus:$other" > /dev/null
move relative 1 0
assert_focus "$other"
move move "$ox" "$oy"
assert_focus "$other"
control u
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null

# A data-device drag can cross the focused game's bounds while it owns the seat grab.
"$ANDROMEDA_DRAG_CLIENT" > "$ANDROMEDA_RUNTIME_DIR/drag.log" 2>&1 &
await_events "$ANDROMEDA_RUNTIME_DIR/drag.log" 'ready$' 1
pointer_release
pointer_hold 2560 720 move 32 32 pause 300 press 272 -- move "$cx" "$cy" move "$ox" "$oy" release 272
await_events "$ANDROMEDA_RUNTIME_DIR/drag.log" 'drag-started$' 1
pointer_release
await_events "$ANDROMEDA_RUNTIME_DIR/drag.log" 'drag-\(finished\|cancelled\)$' 1
assert_focus "$other"
pointer_hold 2560 720 pause 300
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null

# Floating geometry follows resize and compositor moves without trapping the grab.
"$ANDROMEDA" msg window-toggle-floating > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
floating_geometry() {
  "$ANDROMEDA" windows --json | jq -r --arg id "$game" '.[] | select(.id == $id) | "\(.x) \(.y) \(.w) \(.h)"'
}
read -r fx fy fw fh < <(floating_geometry)
move relative 2000 0
assert_position "$((fw - 1))" "$fw" 0 "$fh"
move move "$((fx + fw - 20))" "$((fy + fh / 2))" mod logo press 273 relative 100 0 release 273 mod none
"$ANDROMEDA" settle
read -r nx ny nw nh < <(floating_geometry)
((nw > fw)) || { echo 'confined floating window did not resize'; exit 1; }
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
move mod logo press 272 relative 150 50 release 272 mod none
"$ANDROMEDA" settle
read -r mx my mw mh < <(floating_geometry)
((mx != nx || my != ny)) || { echo 'confined floating window did not move'; exit 1; }
"$ANDROMEDA" msg window-toggle-floating > /dev/null
"$ANDROMEDA" settle

# Fullscreen must stop at the shared output edge, for relative and absolute devices.
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
move relative 2000 0
assert_position 1279 1280 0 720
move move 1920 360
assert_position 1279 1280 0 720
cursor_output() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.focused) | .output'; }
[[ $(cursor_output) == HEADLESS-1 ]]
"$ANDROMEDA" msg output-focus-right > /dev/null
[[ $(cursor_output) == HEADLESS-2 ]]
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle

# Session lock releases confinement; the pointer can reach the other monitor.
mkfifo "$ANDROMEDA_RUNTIME_DIR/lock-control"
exec 9<> "$ANDROMEDA_RUNTIME_DIR/lock-control"
"$ANDROMEDA_LOCK_CLIENT" <&9 > "$ANDROMEDA_RUNTIME_DIR/lock.log" 2>&1 &
await_events "$ANDROMEDA_RUNTIME_DIR/lock.log" 'locked$' 1
move move 1920 360
[[ $(cursor_output) == HEADLESS-2 ]]
echo unlock >&9
await_events "$ANDROMEDA_RUNTIME_DIR/lock.log" 'unlocked$' 1
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null

# Overview and workspace navigation must remain available.
"$ANDROMEDA" msg overview-toggle > /dev/null
move move 1920 360
[[ $(cursor_output) == HEADLESS-2 ]]
"$ANDROMEDA" msg overview-toggle > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
"$ANDROMEDA" msg workspace-next > /dev/null
move move "$ox" "$oy"
"$ANDROMEDA" msg "window-focus-warp:$other" > /dev/null
assert_focus "$other"

# Scaling the home output changes the logical bounds of an already mapped game.
sed -i '/^\[output.HEADLESS-1\]$/a scale = 1.25' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
move relative 2000 0
assert_position 1023 1024 0 576
move relative 0 2000
assert_position 0 1024 575 576
[[ $(cursor_output) == HEADLESS-1 ]]
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
sed -i '/^scale = 1.25$/d' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle

# Destroying the confined window must leave the remaining client usable.
"$ANDROMEDA" msg "window-focus-warp:$game" > /dev/null
kill "$game_pid"
wait_windows 1
move move "$ox" "$oy" click 272
assert_focus "$other"
echo 'rule confinement, client constraints, focus escape and reload passed'
