#!/usr/bin/env bash
# xdg_toplevel_icon_manager_v1 is offered, and the icon name a window sets is reported over IPC.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/icon.log"

"$CLIENT" icon > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the icon window"
name=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "icon") | .icon_name')
if [[ $name != utilities-terminal ]]; then
  echo "IPC reports icon '$name', not utilities-terminal"
  exit 1
fi
echo "toplevel icon utilities-terminal reported over IPC"
