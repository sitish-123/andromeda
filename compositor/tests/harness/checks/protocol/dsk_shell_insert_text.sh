#!/usr/bin/env bash
# dsk_shell_v1 insert_text reaches a focused text-input-v3 client as a commit_string.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly INPUT_METHOD="${ANDROMEDA_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly INPUT_METHOD_LOG="$ANDROMEDA_RUNTIME_DIR/input-method.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"

mkfifo "$SHELL_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
await_events "$INPUT_METHOD_LOG" grabbed 1 "the input method"
ENABLE_TEXT_INPUT=1 "$OBSERVER" text-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" text-input-enter 1 "the text input"
"$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

echo 'insert héllo wörld' >&"$shell_fd"
await_events "$WINDOW_LOG" "text-input-commit-string 'héllo wörld'" 1 "the committed text"
echo "dsk_shell insert_text arrived as a commit_string"
