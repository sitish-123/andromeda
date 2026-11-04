#!/usr/bin/env bash
# dsk_shape_v1.loop moves a shape every frame with no client commit, and the output commits nothing while the
# looping surface is fully transparent: a hidden loop costs no frames.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-shape-loop.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-shape-loop.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-shape-loop.png"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[colors]
backdrop = "#FFFFFFFF"
CONF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_DSK_VERSION=12 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1

# send "<action> <label> ...": waits for the client's "done <label>": it takes one command per frame.
send() {
  printf '%s\n' "$1" >&"$control_fd"
  local label=${1#* }
  await_lines "$LOG" "done ${label%% *} " 1
}
# Red pixels of the 20 px wide bar the shape draws at x 100.
red() {
  grim "$SHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'r > 0.8 && g < 0.3 && b < 0.3' "20x40+100+0"
}
commits() { "$ANDROMEDA" output-commits --json | jq -r '."HEADLESS-1" // .ok."HEADLESS-1"'; }

send "color clear 00000000"
send "dsk-shape fill 10 1 0 0 1"
send "dsk-shape low 3 100 18 20 4"
"$ANDROMEDA" settle
low=$(red)
if ((low == 0 || low > 100)); then
  echo "the 20x4 shape shows $low red pixels before the loop"
  exit 1
fi

# Sine, 400 ms period: from 4 px tall to the full 40 px and back. Grab until two sizes differ.
send "dsk-shape-loop loop 1 3 0 400 0 100 0 20 40"
sizes=()
for _ in 1 2 3 4 5 6; do
  sizes+=("$(red)")
  sleep 0.07 # real time: the loop runs on the compositor clock
done
if [[ $(printf '%s\n' "${sizes[@]}" | sort -u | wc -l) -lt 3 ]]; then
  echo "the looping shape did not move: red pixel counts ${sizes[*]}"
  exit 1
fi

# Fully transparent: no frames, so no commits.
send "dsk-set hide 2 0 0 0 0"
sleep 0.2 # real time: frames in flight finish
before=$(commits)
sleep 1 # real time: nothing may commit within the window
idle=$(($(commits) - before))
if ((idle != 0)); then
  echo "a transparent looping surface committed $idle frames in 1 s"
  exit 1
fi

send "dsk-set show 2 1 0 0 0"
sleep 0.3 # real time: the loop resumes
if (($(commits) - before < 5)); then
  echo "the loop did not resume after the surface showed again"
  exit 1
fi
echo "loop sizes ${sizes[*]}; hidden commits 0"
