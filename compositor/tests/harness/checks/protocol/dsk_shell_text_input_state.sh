#!/usr/bin/env bash
# dsk_shell_v1 text_input_state follows the focused text-input-v3 client: enable with content type and cursor
# rectangle, then disable when the client leaves.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly INPUT_METHOD="${ANDROMEDA_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly INPUT_METHOD_LOG="$ANDROMEDA_RUNTIME_DIR/input-method.log"
# zwp_text_input_v3 content_hint.spellcheck and content_purpose.url
readonly HINT_SPELLCHECK=2
readonly PURPOSE_URL=5

"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
await_events "$INPUT_METHOD_LOG" grabbed 1 "the input method"
"$SHELL_CLIENT" shell-events > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"
if grep -q '^text-input-state enabled=1' "$SHELL_LOG"; then
  echo "text input reported enabled before any client enabled it"
  exit 1
fi

ENABLE_TEXT_INPUT=1 TEXT_INPUT_LATE_STATE=1 TEXT_INPUT_CURSOR_RECT=10,20,3,16 "$OBSERVER" text-window \
  > "$WINDOW_LOG" 2>&1 &
window_pid=$!
await_events "$SHELL_LOG" "text-input-state enabled=1 purpose=$PURPOSE_URL hint=$HINT_SPELLCHECK" 1 "the enable report"
rect=$(grep -m1 "^text-input-state enabled=1 purpose=$PURPOSE_URL" "$SHELL_LOG" | sed 's/.*rect=//')
IFS=, read -r x y w h <<< "$rect"
if ((w != 3 || h != 16 || x < 10 || y < 20)); then
  echo "cursor rectangle $rect does not carry the client's 3x16 rectangle at (10,20) plus the window origin"
  exit 1
fi

kill "$window_pid"
wait "$window_pid" || true
await_events "$SHELL_LOG" "text-input-state enabled=0 purpose=0 hint=0 rect=0,0,0,0" 1 "the disable report"
echo "dsk_shell text_input_state followed the text input enable and disable"
