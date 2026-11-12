#!/usr/bin/env bash
# Reduce motion (rulebook 7.8): a touchpad swipe that lands on another desktop settles the filmstrip on `close` from
# where the fingers left it. It does not jump and crossfade: some frame draws the card at full colour between its release
# place and its settled place.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BLUE='b > 0.6 && r < 0.1 && g < 0.1'
readonly FRAMES=24 STEP_MS=16
IMAGE="$ANDROMEDA_RUNTIME_DIR/swipe.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[appearance]
reduce_motion = true
[overview]
style = "strip"
zoom = 0.5
background_blur = false
[output."HEADLESS-1"]
workspace_axis = "vertical"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

active_workspace() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .index'; }
wait_title() {
  for _ in $(seq 80); do
    [[ -n $("$ANDROMEDA" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .id') ]] && return 0
    sleep 0.025
  done
  echo "$1 never mapped"
  exit 1
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 1200 700 > /dev/null 2>&1 &
wait_title red
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" blue 1200 700 > /dev/null 2>&1 &
wait_title blue
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 move 640 360
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null

# Most of a row along the workspace axis, then the release: it lands on desktop 2.
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA_POINTER_CLIENT" 1280 720 axis vertical 210 pause 200 axis-stop vertical
for _ in $(seq 60); do
  [[ $(active_workspace) == 2 ]] && break
  sleep 0.025
done
[[ $(active_workspace) == 2 ]] || { "$ANDROMEDA" clock-resume > /dev/null; echo "the swipe did not land on desktop 2"; exit 1; }

ys=() levels=()
for _ in $(seq "$FRAMES"); do
  "$ANDROMEDA" clock-advance "$STEP_MS" > /dev/null
  grim -l 0 -o HEADLESS-1 "$IMAGE"
  read -r _ y _ h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
  read -r _ _ level < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "1280x720+0+0")
  ys+=("$((h > 0 ? y : -1))")
  levels+=("$level")
done
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -l 0 -o HEADLESS-1 "$IMAGE"
read -r _ final _ _ < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")

start=${ys[0]} between=0
for i in "${!ys[@]}"; do
  y=${ys[i]}
  ((y < 0 || levels[i] < 240)) && continue
  # Strictly between the first drawn place and the settled one, at full colour: the row glides.
  if (((y - start) * (final - y) > 0 && (y - start > 4 || start - y > 4) && (final - y > 4 || y - final > 4))); then
    between=1
  fi
done
((between)) || { echo "the row jumped instead of settling: blue y per frame ${ys[*]} (level ${levels[*]}), settled $final"; exit 1; }
echo "the released row settles from the fingers to desktop 2: blue y ${ys[*]} -> $final"
