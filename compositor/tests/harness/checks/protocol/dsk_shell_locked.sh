#!/usr/bin/env bash
# While the session is locked, dsk_shell_v1 key and insert_text are dropped: neither the lock screen nor the window
# behind it receives a key. After the unlock the same requests reach the window again (the control).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly LOCK_LOG="$ANDROMEDA_RUNTIME_DIR/lock-client.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly LOCK_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-control"
readonly KEYSYM_X=120
readonly KEYSYM_Y=121
readonly KEY_Y=21

mkfifo "$SHELL_FIFO" "$LOCK_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$OBSERVER" key-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window's keyboard focus"
"$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"
LOG_LOCK_INPUT=1 "$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
await_events "$LOCK_LOG" locked 1 "the lock"
await_events "$LOCK_LOG" keyboard-enter 1 "the lock surface's keyboard focus"

printf 'key %s 1\nkey %s 0\ninsert abc\nmark locked-sent\n' "$KEYSYM_X" "$KEYSYM_X" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ locked-sent 1 "the shell's request round trip"
if [[ $(events "$LOCK_LOG" keyboard-key) != 0 || $(events "$WINDOW_LOG" keyboard-key) != 0 ]]; then
  echo "a request typed while locked: lock=$(tr '\n' '|' < "$LOCK_LOG") window=$(tr '\n' '|' < "$WINDOW_LOG")"
  exit 1
fi

echo unlock >&"$lock_fd"
await_events "$LOCK_LOG" unlocked 1 "the unlock"
await_events "$WINDOW_LOG" keyboard-enter 2 "the window's focus after the unlock"
printf 'key %s 1\nkey %s 0\nmark unlocked-sent\n' "$KEYSYM_Y" "$KEYSYM_Y" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ unlocked-sent 1 "the shell's second round trip"
await_events "$WINDOW_LOG" "keyboard-key code=$KEY_Y state=pressed" 1 "the control: the key after the unlock"
echo "dsk_shell key and insert_text were dropped while locked and worked after the unlock"
