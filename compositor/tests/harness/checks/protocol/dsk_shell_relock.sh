#!/usr/bin/env bash
# dsk_shell_v1 relock arrives when the lock client dies and the session stays locked; recovered arrives when a new
# lock client confirms the lock.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/lock-first.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/lock-second.log"
readonly FIRST_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-first-control"
readonly SECOND_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-second-control"

"$SHELL_CLIENT" shell-events > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

mkfifo "$FIRST_FIFO" "$SECOND_FIFO"
exec {first_fd}<> "$FIRST_FIFO"
"$LOCK_CLIENT" <&"$first_fd" > "$FIRST_LOG" 2>&1 &
first_pid=$!
await_events "$FIRST_LOG" locked 1 "the first lock"
if [[ $(events "$SHELL_LOG" relock) != 0 || $(events "$SHELL_LOG" recovered) != 0 ]]; then
  echo "relock or recovered arrived before any lock client died"
  exit 1
fi

kill -KILL "$first_pid"
wait "$first_pid" || true
await_events "$SHELL_LOG" relock 1 "relock after the lock client died"
if [[ $(events "$SHELL_LOG" recovered) != 0 ]]; then
  echo "recovered arrived before a new lock client took over"
  exit 1
fi

exec {second_fd}<> "$SECOND_FIFO"
"$LOCK_CLIENT" <&"$second_fd" > "$SECOND_LOG" 2>&1 &
second_pid=$!
await_events "$SECOND_LOG" locked 1 "the second lock"
await_events "$SHELL_LOG" recovered 1 "recovered after a new lock client"

echo unlock >&"$second_fd"
await_events "$SECOND_LOG" unlocked 1 "the unlock"
wait "$second_pid"
if [[ $(events "$SHELL_LOG" relock) != 1 ]]; then
  echo "a clean unlock sent relock"
  exit 1
fi
echo "dsk_shell relock followed the lock client's death and recovered its replacement"
