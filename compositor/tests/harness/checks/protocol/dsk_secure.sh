#!/usr/bin/env bash
# harness: outputs=2
# dsk_surface_v1.set_secure: a screenshot shows a secure layer surface as black; clearing it shows the content again.
# A capture of an output that mirrors it, a magnified frame and the surface's popup stay black too, and a window
# capture of a window with a secure subsurface shows none of it.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-secure.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-secure.png"
readonly WINDOW_SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-secure-window.png"
readonly CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-capture.log"
readonly CAPTURE_FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-secure-capture.fifo"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-window.log"
readonly REGION="400x20+100+10"
readonly OUTPUT_REGION="1280x720+0+0"
readonly BLUE='b > 0.5 && r < 0.3'
readonly BLACK='r < 0.05 && g < 0.05 && b < 0.05'

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

BASELINE=$(< "$ANDROMEDA_CONFIG")
cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false

[colors]
backdrop = "#FFFFFFFF"
CONF
CONFIG=$(< "$ANDROMEDA_CONFIG")
"$ANDROMEDA" msg config-reload > /dev/null

pids=()
trap 'kill "${pids[@]}" 2> /dev/null || true' EXIT

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_DSK_VERSION=14 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
pids+=($!)
await_lines "$LOG" ready 1

# Prints "<blue pixels> <black pixels>" in a region (default: the bar strip) of a fresh screenshot of HEADLESS-1.
probe() {
  "$ANDROMEDA" settle
  grim -o HEADLESS-1 "$SHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SHOT" count "$BLUE" "$BLACK" "${1:-$REGION}"
}

# Sends one control command and waits for its "done LABEL".
layer() {
  local label
  read -r _ label _ <<< "$1"
  printf '%s\n' "$1" >&"$control_fd"
  await_lines "$LOG" "done $label " 1
}

layer "color plain FF2040A0"
read -r blue black < <(probe)
if ((blue < 7000)); then
  echo "the bar does not show in a screenshot before set_secure: $blue of 8000 pixels blue"
  exit 1
fi

layer "dsk-secure hide 1"
read -r hidden_blue hidden_black < <(probe)
if ((hidden_blue != 0 || hidden_black < 7000)); then
  echo "a secure bar shows in a screenshot: $hidden_blue blue, $hidden_black black of 8000 pixels"
  exit 1
fi

layer "dsk-secure show 0"
read -r shown_blue shown_black < <(probe)
if ((shown_blue < 7000)); then
  echo "the bar stays hidden after set_secure 0: $shown_blue of 8000 pixels blue"
  exit 1
fi

# Mirroring: a capture session on HEADLESS-2 outlives its wl_output when HEADLESS-2 starts to mirror HEADLESS-1, and
# then reads the mirror's frames. Its top-left pixel is the bar's.
"$ANDROMEDA_POINTER_CLIENT" 100 100 move 25 50 > /dev/null
mkfifo "$CAPTURE_FIFO"
CAPTURE_PIXELS=1 CAPTURE_CONTROL=1 "$ANDROMEDA_CAPTURE_CLIENT" --output HEADLESS-2 < "$CAPTURE_FIFO" > "$CAPTURE_LOG" 2>&1 &
pids+=($!)
exec {capture_fd}> "$CAPTURE_FIFO"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-2' 1
printf '%s\n\n[output.HEADLESS-2]\nmirror = "HEADLESS-1"\n' "$CONFIG" > "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
layer "color mirror-shown FF2040A0"
if ! await_lines "$CAPTURE_LOG" 'pixels HEADLESS-2 uniform=0 color=..2040a0' 1 > /dev/null; then
  echo "a capture of the mirroring output never showed the bar: $(grep '^pixels' "$CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
layer "dsk-secure mirrored 1"
if ! await_lines "$CAPTURE_LOG" 'pixels HEADLESS-2 uniform=0 color=..000000' 1 > /dev/null; then
  echo "a capture of an output mirroring a secure bar shows it: $(grep '^pixels' "$CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
exec {capture_fd}>&-
printf '%s\n' "$CONFIG" > "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

# Magnifier: three zoom-in steps at the top-left corner draw the bar twice as tall, over the strip below it.
"$ANDROMEDA_POINTER_CLIENT" 1 1 move 0 0 > /dev/null
for _ in 1 2 3; do "$ANDROMEDA" msg zoom-in > /dev/null; done
read -r zoomed_blue zoomed_black < <(probe "400x20+100+50")
"$ANDROMEDA" msg zoom-reset > /dev/null
if ((zoomed_blue != 0 || zoomed_black < 7000)); then
  echo "a magnified secure bar shows in a screenshot: $zoomed_blue blue, $zoomed_black black of 8000 pixels"
  exit 1
fi

# A popup of a secure surface is secure too.
layer "popup menu 100"
await_lines "$LOG" popup-mapped 1
read -r popup_blue popup_black < <(probe "60x30+110+45")
if ((popup_blue != 0 || popup_black < 1700)); then
  echo "the popup of a secure bar shows in a screenshot: $popup_blue blue, $popup_black black of 1800 pixels"
  exit 1
fi

# Window capture: the blue subsurface covers the whole window and is secure, its red parent is not.
SECURE=child "$ANDROMEDA_SUBSURFACE_CLIENT" secure-child > "$WINDOW_LOG" 2>&1 &
pids+=($!)
await_lines "$WINDOW_LOG" mapped 1
"$ANDROMEDA" settle
window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "secure-child") | .id')
if ! timeout 10 grim -T "$window_id" "$WINDOW_SHOT" 2> "$WINDOW_LOG.grim"; then
  echo "grim cannot capture the window with a secure subsurface: $(tail -1 "$WINDOW_LOG.grim")"
  exit 1
fi
read -r window_blue window_black < <("$ANDROMEDA_PIXEL_PROBE" "$WINDOW_SHOT" count "$BLUE" "$BLACK")
if ((window_blue != 0 || window_black < 10000)); then
  echo "a window capture is not the black frame of a secure subsurface: $window_blue blue, $window_black black pixels"
  exit 1
fi

# Overview: the cards copy the window's buffers, and an output capture must paint them black too.
"$ANDROMEDA" msg overview-open > /dev/null
read -r overview_blue overview_black < <(probe "$OUTPUT_REGION")
"$ANDROMEDA" msg overview-close > /dev/null
if ((overview_blue != 0)); then
  echo "an overview card shows a secure subsurface in a screenshot: $overview_blue blue pixels"
  exit 1
fi

# A window capture that runs while a secure subsurface maps: the child hides, the capture starts, the child shows.
LATE_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-late.log"
LATE_FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-secure-late.fifo"
LATE_CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-late-capture.log"
mkfifo "$LATE_FIFO"
exec {late_fd}<> "$LATE_FIFO"
SECURE=child CHILD_ON_STDIN=1 "$ANDROMEDA_SUBSURFACE_CLIENT" secure-late <&"$late_fd" > "$LATE_LOG" 2>&1 &
pids+=($!)
await_lines "$LATE_LOG" mapped 1
printf h >&"$late_fd"
await_lines "$LATE_LOG" child-hidden 1
"$ANDROMEDA" settle
late_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "secure-late") | .id')
CAPTURE_PIXELS=1 "$ANDROMEDA_CAPTURE_CLIENT" --window "$late_id" > "$LATE_CAPTURE_LOG" 2>&1 &
pids+=($!)
if ! await_lines "$LATE_CAPTURE_LOG" 'pixels .* color=ffff0000' 1 > /dev/null; then
  echo "a window capture never showed the red parent: $(grep '^pixels' "$LATE_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
printf s >&"$late_fd"
await_lines "$LATE_LOG" child-shown 1
# The frame after the child shows: black when redacted, blue when it leaks.
if ! await_lines "$LATE_CAPTURE_LOG" 'pixels .* color=ff0000\(00\|ff\)' 1 > /dev/null; then
  echo "a window capture got no frame after the secure child showed: $(grep '^pixels' "$LATE_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
late_blue=$(grep -c '^pixels .* color=ff0000ff' "$LATE_CAPTURE_LOG" || true)
if ((late_blue != 0)); then
  echo "a running window capture shows a secure subsurface mapped after it started: $late_blue blue frames"
  exit 1
fi
# Destroying the secure child shows the red parent again.
late_red=$(grep -c '^pixels .* color=ffff0000' "$LATE_CAPTURE_LOG" || true)
printf x >&"$late_fd"
await_lines "$LATE_LOG" child-destroyed 1
if ! await_lines "$LATE_CAPTURE_LOG" 'pixels .* color=ffff0000' $((late_red + 1)) > /dev/null; then
  echo "a window capture stays black after its secure subsurface is destroyed: $(grep '^pixels' "$LATE_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi

# The same for a new secure subsurface of a popup: the window has no secure surface until it maps.
POPUP_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-popup.log"
POPUP_FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-secure-popup.fifo"
POPUP_CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure-popup-capture.log"
mkfifo "$POPUP_FIFO"
exec {popup_fd}<> "$POPUP_FIFO"
CHILD_ON_STDIN=1 "$ANDROMEDA_SUBSURFACE_CLIENT" secure-popup <&"$popup_fd" > "$POPUP_LOG" 2>&1 &
pids+=($!)
await_lines "$POPUP_LOG" mapped 1
printf p >&"$popup_fd"
await_lines "$POPUP_LOG" popup-mapped 1
"$ANDROMEDA" settle
popup_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "secure-popup") | .id')
CAPTURE_PIXELS=1 "$ANDROMEDA_CAPTURE_CLIENT" --window "$popup_id" > "$POPUP_CAPTURE_LOG" 2>&1 &
pids+=($!)
if ! await_lines "$POPUP_CAPTURE_LOG" 'pixels .* color=ff0000ff' 1 > /dev/null; then
  echo "a window capture never showed the window: $(grep '^pixels' "$POPUP_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
printf n >&"$popup_fd"
await_lines "$POPUP_LOG" popup-child-mapped 1
if ! await_lines "$POPUP_CAPTURE_LOG" 'pixels .* uniform=1 color=ff000000' 1 > /dev/null; then
  echo "a window capture shows a secure subsurface of a popup: $(grep '^pixels' "$POPUP_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
echo "plain $blue blue/$black black; secure $hidden_blue/$hidden_black; cleared $shown_blue/$shown_black;" \
  "zoomed $zoomed_blue/$zoomed_black; popup $popup_blue/$popup_black; window capture $window_blue/$window_black;" \
  "overview $overview_blue/$overview_black; late child $late_blue blue frames"
