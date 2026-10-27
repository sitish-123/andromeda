#!/usr/bin/env bash
# Overview shortcut keys focus the labeled card, close the overview, and never leak to clients. Multi-key labels use
# sequential plain key presses, while disabling shortcuts leaves the overview open and continues swallowing input.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/shortcut-first.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/shortcut-second.log"
readonly THIRD_LOG="$ANDROMEDA_RUNTIME_DIR/shortcut-third.log"
readonly FOURTH_LOG="$ANDROMEDA_RUNTIME_DIR/shortcut-fourth.log"
BASELINE=$(< "$ANDROMEDA_CONFIG")

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
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

focused_title() {
  "$ANDROMEDA" windows --json | jq -r '[.[] | select(.focused) | .title] | if length == 1 then .[0] else "none" end'
}

active_title() {
  "$ANDROMEDA" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

focus_title() {
  local title=$1 id
  id=$("$ANDROMEDA" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id')
  if [[ -z $id ]]; then
    echo "could not resolve window '$title': $("$ANDROMEDA" windows --json)"
    return 1
  fi
  "$ANDROMEDA" msg "window-focus:$id" > /dev/null
}

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation.overview]\nduration_ms = 100\n'
    if [[ $# -gt 0 ]]; then
      printf '\n[overview]\n%s\n' "$1"
    fi
  } > "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  finish
}

# Animation time only moves by clock-advance; 2000 ms finishes any animation in this check.
finish() {
  "$ANDROMEDA" clock-advance 2000
}

"$ANDROMEDA" clock-freeze
"$CLIENT" shortcut-first 1200 700 > "$FIRST_LOG" 2>&1 &
wait_for_count 1
"$CLIENT" shortcut-second 1200 700 > "$SECOND_LOG" 2>&1 &
wait_for_count 2
"$CLIENT" shortcut-third 1200 700 > "$THIRD_LOG" 2>&1 &
wait_for_count 3
focus_title shortcut-first
finish

write_config
"$ANDROMEDA" msg overview-open > /dev/null
finish
# One virtual keyboard sends both keys, so the key after the overview closes reaches the client it focused. The pause
# only has to outlast the clock-advance that finishes the close.
pointer tap 4 pause 300 tap 30 &
pointer_pid=$!
for _ in $(seq 40); do
  [[ $(focused_title) == shortcut-third ]] && break
  sleep 0.025
done
if [[ $(focused_title) != shortcut-third ]]; then
  echo "single-key shortcut did not focus the third card: $("$ANDROMEDA" windows --json)"
  exit 1
fi
finish
wait "$pointer_pid"
for _ in $(seq 40); do
  grep -q '^key 30 1$' "$THIRD_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -q '^key 30 1$' "$THIRD_LOG" 2>/dev/null; then
  echo "keyboard focus was not restored after shortcut selection: $(< "$THIRD_LOG")"
  exit 1
fi
if grep -q '^key 4 1$' "$FIRST_LOG" "$SECOND_LOG" "$THIRD_LOG" 2>/dev/null; then
  echo "overview shortcut key leaked to a client"
  exit 1
fi
if grep -q '^key 30 1$' "$FIRST_LOG" "$SECOND_LOG" 2>/dev/null; then
  echo "post-overview input reached a client other than the focused third card"
  exit 1
fi

write_config 'shortcut_keys = "12"'
"$ANDROMEDA" msg overview-open > /dev/null
finish
pointer tap 3 tap 2
finish
if [[ $(focused_title) != shortcut-second ]]; then
  echo "multi-key shortcut 21 did not focus the second card: $("$ANDROMEDA" windows --json)"
  exit 1
fi

write_config 'shortcuts = false'
"$ANDROMEDA" msg overview-open > /dev/null
finish
pointer tap 2
finish
if [[ $(focused_title) != shortcut-second ]]; then
  echo "disabled overview shortcuts changed focus: $("$ANDROMEDA" windows --json)"
  exit 1
fi
if grep -q '^key 2 1$' "$FIRST_LOG" "$SECOND_LOG" "$THIRD_LOG" 2>/dev/null; then
  echo "disabled overview shortcut input leaked to a client"
  exit 1
fi
"$ANDROMEDA" msg overview-close > /dev/null

write_config
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
finish
"$ANDROMEDA" msg overview-open > /dev/null
finish
"$CLIENT" shortcut-fourth 1200 700 > "$FOURTH_LOG" 2>&1 &
wait_for_count 4
finish
pointer tap 5
finish
if [[ $(active_title) != shortcut-fourth ]]; then
  echo "a new window stole an existing badge instead of receiving shortcut 4: $("$ANDROMEDA" windows --json)"
  exit 1
fi

write_config
focus_title shortcut-first
finish
"$ANDROMEDA" msg overview-open > /dev/null
finish
pointer move 620 360 press "$BTN_LEFT" move 760 360 release "$BTN_LEFT"
focus_title shortcut-second
finish
pointer tap 2
finish
if [[ $(active_title) != shortcut-first ]]; then
  echo "the dragged card lost its shortcut badge after drop: $("$ANDROMEDA" windows --json)"
  exit 1
fi

echo "overview shortcuts keep stable IDs through mapping, scrolling, and drag-drop while honoring opt-out"
