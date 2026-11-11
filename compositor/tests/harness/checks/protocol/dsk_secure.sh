#!/usr/bin/env bash
# dsk_surface_v1.set_secure: a screenshot shows a secure layer surface as black; clearing it shows the content again.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-secure.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-secure.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-secure.png"
readonly REGION="400x20+100+10"

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
LAYER_DSK_VERSION=14 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1

# Prints "<blue pixels> <black pixels>" in the bar strip of a fresh screenshot.
probe() {
  "$ANDROMEDA" settle
  grim "$SHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'b > 0.5 && r < 0.3' 'r < 0.05 && g < 0.05 && b < 0.05' "$REGION"
}

printf '%s\n' "color plain FF2040A0" >&"$control_fd"
await_lines "$LOG" 'done plain' 1
read -r blue black < <(probe)
if ((blue < 7000)); then
  echo "the bar does not show in a screenshot before set_secure: $blue of 8000 pixels blue"
  exit 1
fi

printf '%s\n' "dsk-secure hide 1" >&"$control_fd"
await_lines "$LOG" 'done hide' 1
read -r hidden_blue hidden_black < <(probe)
if ((hidden_blue != 0 || hidden_black < 7000)); then
  echo "a secure bar shows in a screenshot: $hidden_blue blue, $hidden_black black of 8000 pixels"
  exit 1
fi

printf '%s\n' "dsk-secure show 0" >&"$control_fd"
await_lines "$LOG" 'done show' 1
read -r shown_blue shown_black < <(probe)
if ((shown_blue < 7000)); then
  echo "the bar stays hidden after set_secure 0: $shown_blue of 8000 pixels blue"
  exit 1
fi
echo "plain $blue blue/$black black; secure $hidden_blue/$hidden_black; cleared $shown_blue/$shown_black"
