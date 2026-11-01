#!/usr/bin/env bash
# dsk_surface_v1.set_elevation draws a shadow under a layer surface, and the shadow follows the surface's translate.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-elevation.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-elevation.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-elevation.png"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false

[colors]
backdrop = "#FFFFFFFF"
CONF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1

# Dark pixels in a 400x20 strip that starts 4 px under the bar's bottom edge at y.
dark_below() {
  "$ANDROMEDA" settle
  grim "$SHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'r < 0.9 && g < 0.9 && b < 0.9' "400x20+100+$(($1 + 4))"
}

printf '%s\n' "color opaque FF2040A0" >&"$control_fd"
await_lines "$LOG" 'done opaque' 1
flat=$(dark_below 40)
if ((flat != 0)); then
  echo "the bar casts a shadow before it asks for elevation: $flat dark pixels"
  exit 1
fi

printf '%s\n' "dsk-elevation raise 2" >&"$control_fd"
await_lines "$LOG" 'done raise' 1
raised=$(dark_below 40)
if ((raised < 4000)); then
  echo "elevation e2 shows no shadow under the bar: $raised of 8000 pixels dark"
  exit 1
fi

# property 0 = translate
printf '%s\n' "dsk-set move 0 0 100 0 0" >&"$control_fd"
await_lines "$LOG" 'done move' 1
left=$(dark_below 40)
moved=$(dark_below 140)
if ((left != 0 || moved < 4000)); then
  echo "the shadow does not follow the translated bar: $left dark at the old edge, $moved at the new edge"
  exit 1
fi
echo "elevation e2 shadow: $raised dark pixels; after translate 100: $moved at the new edge, $left at the old"
