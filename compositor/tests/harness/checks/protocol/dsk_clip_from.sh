#!/usr/bin/env bash
# dsk_surface_v1.set_clip_from maps a rect of another layer surface of the client into this surface's coordinates:
# a click inside the mapped rect reaches the clipped surface, a click outside it does not.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-clip-from.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-clip-from.fifo"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

pointer() { "$ANDROMEDA_POINTER_CLIENT" 1280 720 "$@"; }

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
# A 1280x200 bar at the top of the 1280x720 output.
LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 200 log-pointer <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1

# A 100x50 source surface under the bar at output 300,60.
printf '%s\n' "source src 300 60" >&"$control_fd"
await_lines "$LOG" source-mapped 1
await_lines "$LOG" 'done src ' 1
# Source-local 10,10 50x20 is output 310,70 .. 360,90, so bar-local 310,70 .. 360,90 too.
printf '%s\n' "dsk-clip-from clip 10 10 50 20" >&"$control_fd"
await_lines "$LOG" 'done clip ' 1
await_lines "$LOG" 'dsk-clip-mapped 310 70 50 20' 1

# Presses arrive in order. 305,65 is outside the rect but on the source surface, which reports source-local 5,5;
# 500,100 is outside the rect and on nothing; 320,80 is inside it.
pointer move 305 65 press 272 release 272
pointer move 500 100 press 272 release 272
pointer move 320 80 press 272 release 272
await_lines "$LOG" 'pointer-press ' 2
presses=$(grep 'pointer-press ' "$LOG" | tr '\n' '|')
if [[ $presses != "pointer-press 5 5|pointer-press 320 80|" ]]; then
  echo "expected the source at 5,5, then the bar at 320,80 only; got: $presses"
  exit 1
fi
echo "the clip from source-local 10,10 50x20 sits at bar-local 310,70"
