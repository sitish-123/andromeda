#!/usr/bin/env bash
# dsk_surface_v1.launch_from (rulebook P15): after a layer surface names an icon rect, the next app launch token makes
# the icon's pixels move from there to the middle of the usable area, breathe while the app starts, and fade out
# when the launch expires (5 s without a window).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly TOKEN_CLIENT="${ANDROMEDA_ACTIVATION_TOKEN_CLIENT:-./build-debug/tests/activation-token-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-launch-from.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-launch-from.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-launch-from.png"

if [[ ! -x $LAYER_CLIENT ]] || [[ ! -x $TOKEN_CLIENT ]]; then
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
LAYER_DSK_VERSION=11 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1
printf '%s\n' "color blue FF2040A0" >&"$control_fd"
await_lines "$LOG" 'done blue' 1

# Blue pixels of the icon copy in a box around the middle of the usable area (y 40..720 under the 40 px bar).
blue_in() {
  grim "$SHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'b > 0.5 && r < 0.3' "$1"
}
readonly MIDDLE="120x120+580+320"

"$ANDROMEDA" clock-freeze
printf '%s\n' "dsk-launch-from icon 100 0 40 40" >&"$control_fd"
await_lines "$LOG" 'done icon' 1
"$TOKEN_CLIENT" launch-from-app > /dev/null

if (($(blue_in "$MIDDLE") != 0)); then
  echo "the icon is in the middle before it moved"
  exit 1
fi
"$ANDROMEDA" clock-advance 600
travelled=$(blue_in "$MIDDLE")
# 40 px at 1.5x is a 60 px square: 3600 pixels, less the breathing swing.
if ((travelled < 2500)); then
  echo "the icon did not reach the middle: $travelled blue pixels"
  exit 1
fi
"$ANDROMEDA" clock-advance 275
breathing=$(blue_in "$MIDDLE")
if ((breathing == travelled)); then
  echo "the icon does not breathe while the app starts: $breathing blue pixels both times"
  exit 1
fi
# The 5 s launch limit runs on the real clock; the fade runs on the frozen one.
expired=0
for _ in $(seq 80); do
  "$ANDROMEDA" clock-advance 100
  (($(blue_in "$MIDDLE") == 0)) && { expired=1; break; }
  sleep 0.1
done
if ((expired == 0)); then
  echo "the icon stays after the launch expired"
  exit 1
fi
echo "the launch icon moves to the middle, breathes, and fades when the launch expires"
