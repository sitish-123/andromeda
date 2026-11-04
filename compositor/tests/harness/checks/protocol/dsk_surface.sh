#!/usr/bin/env bash
# dsk_surface_manager_v1 is offered; the compositor runs a layer surface's animation, reports it started, ends it as
# retargeted when a new animate replaces it, and as finished when the new one settles.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly GLOBAL_CLIENT="${ANDROMEDA_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-surface.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-surface.fifo"

if [[ ! -x $GLOBAL_CLIENT || ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

"$GLOBAL_CLIENT" dsk_surface_manager_v1 present 8

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT

await_lines "$LOG" ready 1
# property 0 = translate, curve 1 = spring_standard
printf '%s\n' "dsk-animate down 1 0 1 0 100 0 0" >&"$control_fd"
# The client takes the next command after the first frame; a 10 ms poll sends it long before the spring settles.
await_lines "$LOG" 'done down ' 1 0.01
await_lines "$LOG" 'dsk-started 1' 1
printf '%s\n' "dsk-animate back 2 0 1 0 0 0 0" >&"$control_fd"
# reason 1 = retargeted, 0 = finished
await_lines "$LOG" 'dsk-done 1 1' 1
await_lines "$LOG" 'dsk-started 2' 1
await_lines "$LOG" 'dsk-done 2 0' 1
