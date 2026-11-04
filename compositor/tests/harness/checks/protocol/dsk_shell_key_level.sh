#!/usr/bin/env bash
# dsk_shell_v1 key above the base level carries its modifiers: the window sees Shift depressed when the press of 'A' arrives and
# none for 'x', and a modifier keysym changes the seat modifier state.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly KEYSYM_A_UPPER=65
readonly KEYSYM_X=120
readonly KEYSYM_SHIFT_L=65505
readonly KEY_A=30
readonly KEY_X=45
readonly MOD_SHIFT=1

mkfifo "$SHELL_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
LOG_MODIFIERS=1 "$OBSERVER" key-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window's keyboard focus"
"$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

printf 'key %s 1\nkey %s 0\nkey %s 1\nkey %s 0\nmark typed\n' \
  "$KEYSYM_A_UPPER" "$KEYSYM_A_UPPER" "$KEYSYM_X" "$KEYSYM_X" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ typed 1 "the typed keys"
await_events "$WINDOW_LOG" "keyboard-key code=$KEY_X state=released" 1 "the x release"
if ! awk -v a="$KEY_A" -v x="$KEY_X" -v shift="$MOD_SHIFT" '
  /^keyboard-modifiers/ { split($2, d, "="); held = d[2] }
  /^keyboard-key/ {
    split($2, c, "=")
    if (c[2] == a) { sawA++; if ($3 == "state=pressed" && held != shift) bad++ }
    if (c[2] == x) { sawX++; if (held != 0) bad++ }
  }
  END { exit(sawA == 2 && sawX == 2 && bad == 0 ? 0 : 1) }' "$WINDOW_LOG"; then
  echo "'A' did not arrive with Shift, or 'x' did: $(tr '\n' '|' < "$WINDOW_LOG")"
  exit 1
fi

printf 'key %s 1\nmark shift-down\n' "$KEYSYM_SHIFT_L" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ shift-down 1 "the Shift press"
await_events "$WINDOW_LOG" "keyboard-modifiers depressed=$MOD_SHIFT " 2 "Shift held after the Shift_L press"
printf 'key %s 0\nmark shift-up\n' "$KEYSYM_SHIFT_L" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ shift-up 1 "the Shift release"
if ! awk '/^keyboard-modifiers/ { last = $0 } END { exit(last ~ /depressed=0 / ? 0 : 1) }' "$WINDOW_LOG"; then
  echo "Shift_L release left Shift held: $(tr '\n' '|' < "$WINDOW_LOG")"
  exit 1
fi
echo "dsk_shell key sent Shift around 'A', none around 'x', and Shift_L moved the seat modifier state"
