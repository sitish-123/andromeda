#!/usr/bin/env bash
# Reduce motion (rulebook 7.8): what moves in the overview crossfades instead. A reflow (a closed window's neighbour in a
# Spaces exposé) and a row move (Strip, switching to the next desktop) never draw a card between its old and new
# place, and the card fades on the way.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly W=1280 H=720
readonly BLUE='b > 0.6 && r < 0.1 && g < 0.1'
readonly FRAMES=40 STEP_MS=16
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
bbox() {
  grim -o HEADLESS-1 "$IMAGE"
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$1"
}
near() { (($1 - $2 <= 2 && $2 - $1 <= 2)); }

# frames <label> <predicate> <start bbox> <end bbox>: advances the frozen clock frame by frame until the card settles
# and fails if a frame draws the card away from both places. Sets FADED when a frame drew it faded or not at all.
frames() {
  local label=$1 predicate=$2 start=$3 end=$4 x y w h sx sy ex ey peak
  read -r sx sy _ _ <<< "$start"
  read -r ex ey _ _ <<< "$end"
  FADED=0
  for frame in $(seq "$FRAMES"); do
    "$ANDROMEDA" clock-advance "$STEP_MS" > /dev/null
    read -r x y w h < <(bbox "$predicate")
    if ((w == 0)); then
      FADED=1
      continue
    fi
    if ! { near "$x" "$sx" && near "$y" "$sy"; } && ! { near "$x" "$ex" && near "$y" "$ey"; }; then
      "$ANDROMEDA" clock-resume > /dev/null
      echo "$label: at $((frame * STEP_MS)) ms the card is at ${w}x${h}+$x+$y, away from $start and $end"
      exit 1
    fi
    read -r _ _ peak < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" max "${w}x${h}+${x}+${y}")
    ((peak < 230)) && FADED=1
    # Faded and back at full strength in its new place: settled.
    ((FADED && peak >= 250)) && near "$x" "$ex" && near "$y" "$ey" && break
  done
  return 0
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" closing 500 400 > /dev/null 2>&1 &
wait_count 1
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" survivor 500 400 > /dev/null 2>&1 &
wait_count 2
"$ANDROMEDA_POINTER_CLIENT" "$W" "$H" move 5 5
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
start=$(bbox "$BLUE")
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg "window-close:$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "closing") | .id')" > /dev/null
wait_count 1
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
end=$(bbox "$BLUE")
read -r sx _ _ _ <<< "$start"
read -r ex _ _ _ <<< "$end"
near "$sx" "$ex" && { echo "reflow: the survivor did not move ($start -> $end)"; exit 1; }

# The same close again, sampled: reopen with two windows.
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" closing 500 400 > /dev/null 2>&1 &
wait_count 2
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
start=$(bbox "$BLUE")
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg "window-close:$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "closing") | .id')" > /dev/null
wait_count 1
frames reflow "$BLUE" "$start" "$end"
"$ANDROMEDA" clock-resume > /dev/null
((FADED)) || { echo "reflow: the survivor jumped from $start to $end without fading"; exit 1; }
echo "reflow: the survivor crossfades from $start to $end"

# Row move: Strip, red on desktop 1, blue on desktop 2; switching desktops moves both rows.
"$ANDROMEDA" msg overview-close > /dev/null
sed -i 's/^style = "spaces"$/style = "strip"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" other 500 400 > /dev/null 2>&1 &
wait_count 2
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null
red_ws=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "other") | .workspace')
blue_ws=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "survivor") | .workspace')
[[ $red_ws != "$blue_ws" ]] || { echo "row: red and blue share $red_ws"; exit 1; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
start=$(bbox "$BLUE")
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle > /dev/null
end=$(bbox "$BLUE")
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null
[[ $(bbox "$BLUE") == "$start" ]] || { echo "row: switching back did not restore the row"; exit 1; }
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
frames row "$BLUE" "$start" "$end"
"$ANDROMEDA" clock-resume > /dev/null
((FADED)) || { echo "row: the row jumped from $start to $end without fading"; exit 1; }
echo "row: the left desktop's card crossfades from $start to $end"
