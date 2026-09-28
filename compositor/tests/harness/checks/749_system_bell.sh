#!/usr/bin/env bash
# xdg_system_bell_v1 is offered, and a ring reaches the desktop shell as a dsk_shell_v1 bell naming the ringing app.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly SHELL_CLIENT="${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$UMBRIEL_RUNTIME_DIR/shell-events.log"
readonly LOG="$UMBRIEL_RUNTIME_DIR/bell.log"

"$SHELL_CLIENT" shell-events > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"
"$CLIENT" bell ringer > "$LOG" 2>&1 &
await_events "$LOG" rang 1 "the ringing window"
await_events "$SHELL_LOG" "bell ringer" 1 "the shell's bell event"
echo "system bell from ringer reached the shell"
