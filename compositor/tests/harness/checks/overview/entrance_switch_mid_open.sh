#!/usr/bin/env bash
# Spaces overview: which windows grow from the desktop is decided when the open starts. A workspace switch halfway
# through the open does not make the new row's window fly in from its desktop rect: it stays inside its slot.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BLUE='b > 0.4 && r < 0.25 && g < 0.25'
readonly FRAMES=30
IMAGE="$ANDROMEDA_RUNTIME_DIR/switch.png"

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[overview]
style = "spaces"
background_blur = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

wait_title() {
  for _ in $(seq 80); do
    [[ -n $("$ANDROMEDA" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .id') ]] && return 0
    sleep 0.025
  done
  echo "$1 never mapped"
  exit 1
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 600 400 > /dev/null 2>&1 &
wait_title red
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" blue 600 400 > /dev/null 2>&1 &
wait_title blue
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null

# The settled slot of the blue card once its workspace is the chosen row.
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
read -r sx sy sw sh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
((sw > 0)) || { echo "no blue card in the open overview on workspace 2"; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
# About half of the open's travel.
for _ in $(seq 8); do
  "$ANDROMEDA" clock-advance 8 > /dev/null
done
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
for frame in $(seq "$FRAMES"); do
  "$ANDROMEDA" clock-advance 16 > /dev/null
  grim -o HEADLESS-1 "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
  # The row slides the slot in from the side and the thumbs grow above it. A card larger than its slot or below it
  # flew in from its desktop rect, which sits below the shown row.
  if ((w > sw + 2 || h > sh + 2 || y + h > sy + sh + 1)); then
    "$ANDROMEDA" clock-resume > /dev/null
    echo "$((frame * 16)) ms after the switch the blue card is ${w}x${h}+${x}+${y}, larger than or below its slot ${sw}x${sh}+${sx}+${sy}"
    exit 1
  fi
done
"$ANDROMEDA" clock-resume > /dev/null
echo "a switch mid-open keeps the new row's card inside its ${sw}x${sh} slot"
