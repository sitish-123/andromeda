#!/usr/bin/env bash
# A dsk_tracker_v1 on a layer surface claims a gesture over it. A three-finger throw rests on the far snap and a slow
# drag back rests on the nearer one. A button drag claims the pointer after the slop along the axis, a sideways drag
# never does.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

swipe() { "$ANDROMEDA" swipe-test "$@" > /dev/null; }
pointer() { "$ANDROMEDA_POINTER_CLIENT" 1280 720 "$@"; }

# start_tracker NAME FINGERS: a background layer surface with a vertical tracker over range 0..400, snaps 0 and 400.
start_tracker() {
  LOG="$ANDROMEDA_RUNTIME_DIR/$1.log"
  FIFO="$ANDROMEDA_RUNTIME_DIR/$1.fifo"
  mkfifo "$FIFO"
  exec {control_fd}<> "$FIFO"
  LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 0 log-pointer <&"$control_fd" > "$LOG" 2>&1 &
  client_pid=$!
  await_lines "$LOG" ready 1
  printf '%s\n' "dsk-track trk $2 0 400 0 400" >&"$control_fd"
  await_lines "$LOG" 'done trk ' 1
}
stop_tracker() {
  kill "$client_pid" 2> /dev/null || true
  wait "$client_pid" 2> /dev/null || true
  exec {control_fd}>&-
}
trap 'kill "$client_pid" 2> /dev/null || true' EXIT

start_tracker swipe 3
pointer move 640 360
t=1000
swipe begin "$t"
for _ in 1 2 3 4; do
  t=$((t + 8))
  swipe update "$t" 0 25
done
swipe end $((t + 8)) 0
await_lines "$LOG" trk-interacting 1
await_lines "$LOG" 'trk-inertia 400' 1
await_lines "$LOG" 'trk-idle 400' 1

# The tracker moved the layer surface down by 400, so the pointer goes where the surface is now.
pointer move 640 600
t=2000
swipe begin "$t"
for _ in $(seq 20); do
  t=$((t + 24))
  swipe update "$t" 0 -5
done
swipe end $((t + 24)) 0
await_lines "$LOG" 'trk-inertia 400' 2
stop_tracker

start_tracker sideways 0
pointer move 640 360 press 272 move 760 360 move 880 360 release 272
pointer mark sideways-done
! grep -q 'trk-interacting' "$LOG"
stop_tracker

start_tracker drag 0
pointer move 640 360 press 272 move 640 385 move 640 410 move 640 435 move 640 460 release 272
await_lines "$LOG" trk-interacting 1
await_lines "$LOG" pointer-leave 1
await_lines "$LOG" 'trk-inertia 400' 1
