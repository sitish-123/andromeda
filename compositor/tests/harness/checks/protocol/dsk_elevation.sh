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

# property 5 = elevation (version 4): 0 removes the shadow, 2 draws e2 again
printf '%s\n' "dsk-set flat 5 0 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done flat' 1
flat_prop=$(dark_below 140)
printf '%s\n' "dsk-set high 5 2 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done high' 1
high_prop=$(dark_below 140)
if ((flat_prop != 0 || high_prop < 4000)); then
  echo "the elevation property does not drive the shadow: $flat_prop dark at level 0, $high_prop at level 2"
  exit 1
fi
echo "elevation property: level 0 $flat_prop dark pixels, level 2 $high_prop"

# fractional levels and animate: the shadow grows from e1 (1) through 1.5 to e2 (2)
printf '%s\n' "dsk-set one 5 1 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done one' 1
dark_one=$(dark_below 140)
printf '%s\n' "dsk-set half 5 1.5 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done half' 1
dark_half=$(dark_below 140)
if ((dark_one >= dark_half || dark_half >= high_prop)); then
  echo "level 1.5 is not between e1 and e2: $dark_one, $dark_half, $high_prop dark pixels at levels 1, 1.5, 2"
  exit 1
fi
# animate 5 with spring_standard (1); the animation setting is off, so it lands at once
printf '%s\n' "dsk-set zero 5 0 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done zero' 1
printf '%s\n' "dsk-animate grow 7 5 1 1.5 0 0 0" >&"$control_fd"
await_lines "$LOG" 'done grow' 1
await_lines "$LOG" 'dsk-done 7 0' 1
dark_anim=$(dark_below 140)
if ((dark_anim != dark_half)); then
  echo "animate of the elevation property ends at $dark_anim dark pixels, set 1.5 gives $dark_half"
  exit 1
fi
echo "elevation levels 1 / 1.5 / 2: $dark_one / $dark_half / $high_prop dark pixels; animate to 1.5: $dark_anim"

# a client that bound version 3 must not use the version 4 property
readonly OLD_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-elevation-v3.log"
readonly OLD_FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-elevation-v3.fifo"
mkfifo "$OLD_FIFO"
exec {old_fd}<> "$OLD_FIFO"
LAYER_DSK_VERSION=3 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$old_fd" > "$OLD_LOG" 2>&1 &
old_pid=$!
trap 'kill "$client_pid" "$old_pid" 2> /dev/null || true' EXIT
await_lines "$OLD_LOG" ready 1
printf '%s\n' "dsk-set bad 5 1 0 0 0" >&"$old_fd"
await_lines "$OLD_LOG" 'protocol-error' 1
echo "a version 3 client gets a protocol error for the elevation property"
