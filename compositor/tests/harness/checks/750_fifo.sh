#!/usr/bin/env bash
# wp_fifo_manager_v1 is offered, and 60 updates sent at once, each waiting on the barrier the one before set, are
# applied one per refresh instead of all at the next frame.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/fifo.log"

"$CLIENT" fifo > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the fifo window"
await_events "$LOG" "fifo " 1 "the fifo run"
ms=$(sed -n 's/^fifo //p' "$LOG")
# 59 barriers at 60 Hz take about 980 ms; without them every update lands on one frame.
if ((ms < 700 || ms > 3000)); then
  echo "60 fifo updates took $ms ms, not about one refresh each"
  exit 1
fi
echo "60 fifo updates took $ms ms, one per refresh"
