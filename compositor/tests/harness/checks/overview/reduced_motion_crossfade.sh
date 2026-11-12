#!/usr/bin/env bash
# Reduce motion (rulebook 7.8): what moves in the overview crossfades instead. A reflow (a closed window's neighbour in a
# Spaces exposé) and a row move (Strip, switching to the next desktop) never draw a card between its old and new
# place, and the card fades on the way.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly W=1280 H=720
readonly BLUE='b > 0.6 && r < 0.1 && g < 0.1'
readonly FRAMES=20 STEP_MS=32
IMAGE="$ANDROMEDA_RUNTIME_DIR/crossfade.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[appearance]
reduce_motion = true
[layout]
mode = "floating"
[overview]
style = "spaces"
background_blur = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

wait_count() {
  for _ in $(seq 80); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.025
  done
  echo "expected $1 window(s): $("$ANDROMEDA" windows --json)"
  exit 1
}
# bbox [crop x,y wxh]: the blue card's box in output px, from an uncompressed capture of the crop.
bbox() {
  local crop=${1:-0,0 ${W}x${H}} ox oy x y w h
  grim -l 0 -g "$crop" "$IMAGE"
  IFS=, read -r ox oy <<< "${crop%% *}"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
  ((w > 0)) && echo "$((x + ox)) $((y + oy)) $w $h" || echo "0 0 0 0"
}
near() { (($1 - $2 <= 2 && $2 - $1 <= 2)); }

# sample <crop>: advances the frozen clock frame by frame and records the blue card's box and peak blue per frame.
sample() {
  local ox oy box peak x y w h
  IFS=, read -r ox oy <<< "${1%% *}"
  BOXES=()
  for _ in $(seq "$FRAMES"); do
    "$ANDROMEDA" clock-advance "$STEP_MS" > /dev/null
    box=$(bbox "$1")
    read -r x y w h <<< "$box"
    peak=0
    if ((w > 0)); then
      read -r _ _ peak < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "${w}x${h}+$((x - ox))+$((y - oy))")
    fi
    BOXES+=("$box $peak")
  done
}

# check <label> <start box> <end box>: every recorded frame drew the card at its start or end place or not at all, and
# at least one frame drew it faded or hidden.
check() {
  local label=$1 sx sy ex ey x y w h peak faded=0
  read -r sx sy _ _ <<< "$2"
  read -r ex ey _ _ <<< "$3"
  { near "$sx" "$ex" && near "$sy" "$ey"; } && { echo "$label: the card did not move ($2 -> $3)"; exit 1; }
  for i in "${!BOXES[@]}"; do
    read -r x y w h peak <<< "${BOXES[$i]}"
    if ((w == 0 || peak < 230)); then
      faded=1
    fi
    ((w == 0)) && continue
    if ! { near "$x" "$sx" && near "$y" "$sy"; } && ! { near "$x" "$ex" && near "$y" "$ey"; }; then
      echo "$label: at $(((i + 1) * STEP_MS)) ms the card is at ${w}x${h}+$x+$y, away from $2 and $3"
      exit 1
    fi
  done
  ((faded)) || { echo "$label: the card jumped from $2 to $3 without fading"; exit 1; }
  echo "$label: the card crossfades from $2 to $3"
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" closing 500 400 > /dev/null 2>&1 &
wait_count 1
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" survivor 500 400 > /dev/null 2>&1 &
wait_count 2
"$ANDROMEDA_POINTER_CLIENT" "$W" "$H" move 5 5
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
start=$(bbox)
read -r _ sy _ sh <<< "$start"
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg "window-close:$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "closing") | .id')" > /dev/null
wait_count 1
# The exposé moves the survivor along its row.
sample "0,$sy ${W}x$sh"
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
check reflow "$start" "$(bbox)"

# Row move: Strip, blue alone on desktop 1; switching to desktop 2 moves its row.
"$ANDROMEDA" msg overview-close > /dev/null
sed -i 's/^style = "spaces"$/style = "strip"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
start=$(bbox)
read -r sx _ sw _ <<< "$start"
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
# The rows move along the workspace axis, here vertically.
sample "$sx,0 ${sw}x$H"
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
check row "$start" "$(bbox)"
