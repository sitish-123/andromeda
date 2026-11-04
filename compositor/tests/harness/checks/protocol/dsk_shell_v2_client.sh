#!/usr/bin/env bash
# A shell that binds dsk_shell_v1 at version 2 gets no version 3 event: no text_input_state, relock or recovered
# (a v3 event on a v2 object is a protocol error that kills the client). The mark round trip proves the shell is
# still connected after the events would have arrived.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly INPUT_METHOD="${ANDROMEDA_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly INPUT_METHOD_LOG="$ANDROMEDA_RUNTIME_DIR/input-method.log"
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/lock-first.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/lock-second.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly FIRST_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-first-control"
readonly SECOND_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-second-control"

mkfifo "$SHELL_FIFO" "$FIRST_FIFO" "$SECOND_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
exec {first_fd}<> "$FIRST_FIFO"
exec {second_fd}<> "$SECOND_FIFO"
"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
await_events "$INPUT_METHOD_LOG" grabbed 1 "the input method"
DESKTOP_CLIENT_SHELL_VERSION=2 "$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the version 2 shell stand-in"
ENABLE_TEXT_INPUT=1 "$OBSERVER" text-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" text-input-enter 1 "the text input"

"$LOCK_CLIENT" <&"$first_fd" > "$FIRST_LOG" 2>&1 &
first_pid=$!
await_events "$FIRST_LOG" locked 1 "the first lock"
kill -KILL "$first_pid"
wait "$first_pid" || true
"$LOCK_CLIENT" <&"$second_fd" > "$SECOND_LOG" 2>&1 &
await_events "$SECOND_LOG" locked 1 "the second lock"
echo 'mark after' >&"$shell_fd"
await_events "$SHELL_LOG" mark\ after 1 "the shell's round trip"
for event in text-input-state relock recovered; do
  if [[ $(events "$SHELL_LOG" "$event") != 0 ]]; then
    echo "the version 2 shell got $event: $(tr '\n' '|' < "$SHELL_LOG")"
    exit 1
  fi
done
echo "a version 2 shell got no version 3 event"
