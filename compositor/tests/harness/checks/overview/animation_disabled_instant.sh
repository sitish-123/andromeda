#!/usr/bin/env bash
# animation.overview.enabled = false: nothing in the overview animates. The other desktop's card is fully shown on the
# first frame of the open, and a search dims the cards it misses on the first frame after the key.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BLUE='b > 0.4 && r < 0.25 && g < 0.25'
readonly K_R=19 K_E=18 K_D=32
IMAGE="$ANDROMEDA_RUNTIME_DIR/instant.png"

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[animation.overview]
enabled = false
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
# Brightest blue inside the blue card's settled slot.
level() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r _ _ b < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "${sw}x${sh}+${sx}+${sy}")
  echo "$b"
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 600 400 > /dev/null 2>&1 &
wait_title red
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" blue 600 400 > /dev/null 2>&1 &
wait_title blue
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
read -r sx sy sw sh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
((sw > 0)) || { echo "no blue card in the open overview"; exit 1; }
final=$(level)
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" clock-advance 1 > /dev/null
opened=$(level)
"$ANDROMEDA_POINTER_CLIENT" 1280 720 tap $K_R tap $K_E tap $K_D
"$ANDROMEDA" clock-advance 1 > /dev/null
searched=$(level)
"$ANDROMEDA" clock-resume > /dev/null

((opened >= final - 8)) || { echo "the other desktop's card faded in: $opened on the first frame, settled $final"; exit 1; }
((searched < final * 2 / 5)) || { echo "the search dimmed the missed card over time: $searched on the first frame, open $final"; exit 1; }
echo "with overview animation off, the card shows at once ($opened of $final) and a search dims it at once ($searched)"
