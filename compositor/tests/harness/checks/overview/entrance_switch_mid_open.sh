#!/usr/bin/env bash
# Spaces overview: which windows grow from the desktop is decided when the open starts. A workspace switch halfway
# through the open does not make the new row's window fly in from its desktop rect: it stays inside its slot. A close
# before the open settles keeps those classes, so no card jumps.
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
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null

# Switched mid-open, then closed before the open settled: every card keeps the class it opened with, so no card jumps
# between the last frame of the open and the first of the close.
readonly RED='r > 0.4 && g < 0.25 && b < 0.25'
box_of() {
  grim -o HEADLESS-1 "$IMAGE"
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$1"
}
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
for _ in $(seq 10); do
  "$ANDROMEDA" clock-advance 8 > /dev/null
done
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 8 > /dev/null
red0=$(box_of "$RED") blue0=$(box_of "$BLUE")
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" clock-advance 8 > /dev/null
red1=$(box_of "$RED") blue1=$(box_of "$BLUE")
"$ANDROMEDA" clock-resume > /dev/null
# One 8 ms frame moves a card a few dozen pixels at most; a class change moves it by a large part of the screen.
jumped() {
  local -a a=($1) b=($2)
  # A large card that vanishes or pops up within one frame jumped as well.
  ((a[2] > 100 && b[2] == 0 || b[2] > 100 && a[2] == 0)) && return 0
  ((a[2] > 0 && b[2] > 0)) || return 1
  for i in 0 1 2 3; do
    ((a[i] - b[i] > 60 || b[i] - a[i] > 60)) && return 0
  done
  return 1
}
if jumped "$red0" "$red1" || jumped "$blue0" "$blue1"; then
  echo "a close before the open settled made a card jump: red $red0 -> $red1, blue $blue0 -> $blue1 (x y w h) in 8 ms"
  exit 1
fi
echo "a switch mid-open keeps the new row's card inside its ${sw}x${sh} slot; an early close keeps every card in place (red $red0 -> $red1, blue $blue0 -> $blue1)"
