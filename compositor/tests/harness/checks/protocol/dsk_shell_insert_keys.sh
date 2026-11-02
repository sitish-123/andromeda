#!/usr/bin/env bash
# dsk_shell_v1 insert_text with no text input types base-level characters as key presses, maps newline to Return,
# and drops a character the keymap lacks.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly KEY_A=30
readonly KEY_B=48
readonly KEY_ENTER=28

mkfifo "$SHELL_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
"$OBSERVER" key-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window's keyboard focus"
"$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

printf 'insert a漢b\\n\nmark typed\n' >&"$shell_fd"
await_events "$SHELL_LOG" mark\ typed 1 "the typed text"
await_events "$WINDOW_LOG" "keyboard-key code=$KEY_ENTER state=released" 1 "the newline's release"
expected=$(printf 'keyboard-key code=%s state=%s\n' \
  "$KEY_A" pressed "$KEY_A" released "$KEY_B" pressed "$KEY_B" released "$KEY_ENTER" pressed "$KEY_ENTER" released)
actual=$(grep '^keyboard-key' "$WINDOW_LOG")
if [[ $actual != "$expected" ]]; then
  echo "the window saw other keys than a, b and Return: $(tr '\n' '|' <<< "$actual")"
  exit 1
fi
echo "dsk_shell insert_text typed a, b and Return as key presses and dropped a character the keymap lacks"
