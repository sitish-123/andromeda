#!/usr/bin/env bash
# Spaces overview: a window on another desktop does not travel into the overview. It appears in its thumb slot, never
# outside it, and its opacity rises once the shown windows pass 35 % of their travel. entrance_in_place_taskview.sh runs
# it in Task View. With ENTRANCE_SHOTS set to a directory, the frames are copied there.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly STYLE=${ENTRANCE_STYLE:-spaces}
readonly BLUE='b > 0.4 && r < 0.25 && g < 0.25'
readonly FRAMES=24
IMAGE="$ANDROMEDA_RUNTIME_DIR/entrance.png"

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[overview]
style = "$STYLE"
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

# The settled slot of the blue card.
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
read -r sx sy sw sh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
((sw > 0)) || { echo "$STYLE: no blue card in the open overview"; exit 1; }
read -r _ _ final < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "${sw}x${sh}+${sx}+${sy}")
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
blues=()
for frame in $(seq "$FRAMES"); do
  "$ANDROMEDA" clock-advance 8 > /dev/null
  grim -o HEADLESS-1 "$IMAGE"
  [[ -n ${ENTRANCE_SHOTS:-} ]] && cp "$IMAGE" "$ENTRANCE_SHOTS/$STYLE-$((frame * 8))ms.png"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
  if ((w > 0)) && ((x < sx - 1 || y < sy - 1 || x + w > sx + sw + 1 || y + h > sy + sh + 1)); then
    echo "$STYLE: at $((frame * 8)) ms the blue card is at ${w}x${h}+${x}+${y}, outside its slot ${sw}x${sh}+${sx}+${sy}"
    exit 1
  fi
  read -r _ _ b < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "${sw}x${sh}+${sx}+${sy}")
  blues+=("$b")
done
"$ANDROMEDA" clock-resume > /dev/null

# Hidden before the gate, partly shown on the way, and rising to the settled card.
first=${blues[0]} rising=0 partial=0
for b in "${blues[@]}"; do
  ((b > first)) && rising=1
  ((b > first + 20 && b < final - 20)) && partial=1
done
((rising && partial)) || { echo "$STYLE: the blue card did not fade in (max blue per frame: ${blues[*]}, settled $final)"; exit 1; }
echo "$STYLE: the other desktop's card stays in ${sw}x${sh}+${sx}+${sy} while it fades in: ${blues[*]} -> $final"
