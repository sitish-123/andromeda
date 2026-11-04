#!/usr/bin/env bash
# Pointer input follows a dsk_surface_v1 scale and clip: a press on the shown surface reaches the client in its own
# unscaled coordinates, and a press on a part the clip hides does not reach it.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-input.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-input.fifo"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false
CONF
"$ANDROMEDA" msg config-reload > /dev/null

pointer() { "$ANDROMEDA_POINTER_CLIENT" 1280 720 "$@"; }

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
# A 1280x200 bar at the top of the 1280x720 output.
LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 200 log-pointer <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1

# expect_press X Y LOCAL_X LOCAL_Y: a click at output X,Y reaches the bar at LOCAL_X,LOCAL_Y. Presses arrive in order,
# so a click that should not have reached the bar shows here as one press too many.
presses=0
expect_press() {
  pointer move "$1" "$2" press 272 release 272
  presses=$((presses + 1))
  await_lines "$LOG" 'pointer-press ' "$presses"
  local count got
  count=$(grep -c 'pointer-press ' "$LOG")
  got=$(grep 'pointer-press ' "$LOG" | tail -n 1)
  if ((count != presses)); then
    echo "a click that the bar must not get reached it: $(grep 'pointer-press ' "$LOG" | tr '\n' '|')"
    exit 1
  fi
  if [[ $got != "pointer-press $3 $4" ]]; then
    echo "a click at $1,$2 reached the bar as '$got', expected local $3,$4"
    exit 1
  fi
}
# click_outside X Y: a click at output X,Y that the bar must not get; the next expect_press checks it.
click_outside() { pointer move "$1" "$2" press 272 release 272; }

expect_press 480 100 480 100

# property 1 = scale. Half size about the center: the bar shows at x 320..960, y 50..150.
printf '%s\n' "dsk-set half 1 0.5 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done half ' 1
click_outside 100 100
expect_press 480 100 320 100

# property 3 = clip (x y w h, surface-local). Full size again; only x 100..300, y 50..150 shows.
printf '%s\n' "dsk-set full 1 1 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done full ' 1
printf '%s\n' "dsk-set clip 3 100 50 200 100" >&"$control_fd"
await_lines "$LOG" 'done clip ' 1
click_outside 500 100
click_outside 150 180
expect_press 150 80 150 80
echo "presses map through scale 0.5 and stop outside the clip"
