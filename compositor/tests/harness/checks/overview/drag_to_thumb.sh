#!/usr/bin/env bash
# Spaces overview (thumb row above): a window card dropped on another desktop's thumb moves the window to that desktop,
# and one dropped on the + thumb moves it to a new desktop. The dragged card shrinks as the pointer nears the thumb row.
# All at output scale 1 and 2. drag_to_thumb_taskview.sh runs it in Task View (row below) with a floating window, which
# must stay on screen.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272
readonly W=1280 H=720
readonly STYLE=${DRAG_TO_THUMB_STYLE:-spaces}
readonly FLOATING=${DRAG_TO_THUMB_FLOATING:-false}
# Thumb width in the row (style.cpp kSpacesThumbWidth, kTaskViewThumbWidth).
THUMB_W=176
[[ $STYLE == taskview ]] && THUMB_W=208
IMAGE="$ANDROMEDA_RUNTIME_DIR/drag-to-thumb.png"
readonly RED='r > 0.6 && g < 0.3 && b < 0.3'
readonly BLUE='b > 0.6 && r < 0.3 && g < 0.3'

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[animation]
enabled = false
[overview]
style = "$STYLE"
background_blur = false
[[window_rule]]
match.title = "^red$"
default_floating = $FLOATING
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

ws_of() { "$ANDROMEDA" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .workspace'; }
# A window that is on screen has its whole box inside the output.
on_screen() {
  "$ANDROMEDA" windows --json | jq -e --arg t "$1" --argjson w "$W" --argjson h "$H" \
    '.[] | select(.title == $t) | .x >= 0 and .y >= 0 and .x + .w <= $w and .y + .h <= $h' > /dev/null
}
index_of() { "$ANDROMEDA" workspaces --json | jq -r --arg id "$1" '.[] | select(.id == $id) | .index'; }
wait_title() {
  for _ in $(seq 80); do
    [[ -n $(ws_of "$1") ]] && return 0
    sleep 0.025
  done
  echo "$1 never mapped"
  exit 1
}

# probe <scale> <op> <predicate> [region in logical px]: bbox in logical px, or a pixel count.
probe() {
  local scale=$1 op=$2 predicate=$3 region=${4:-}
  grim -o HEADLESS-1 "$IMAGE"
  if [[ -n $region ]]; then
    region=$(awk -v s="$scale" -v r="$region" 'BEGIN {
      split(r, a, /[x+]/); printf "%dx%d+%d+%d", a[1] * s, a[2] * s, a[3] * s, a[4] * s }')
  fi
  if [[ $op == bbox ]]; then
    "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$predicate" ${region:+"$region"} |
      awk -v s="$scale" '{ printf "%d %d %d %d\n", $1 / s, $2 / s, $3 / s, $4 / s }'
  else
    "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$predicate" ${region:+"$region"}
  fi
}

# drag_red <scale> <target x> <target y>: drags the red card from its centre in the main area to the target and
# releases there. Sets AREAS to the red pixel counts "claim mid over" (thumbs draw the shown desktop as a grey stand-in).
drag_red() {
  local scale=$1 tx=$2 ty=$3 rx ry rw rh
  read -r rx ry rw rh < <(probe "$scale" bbox "$RED")
  ((rw > 0)) || { echo "scale $scale: no red card in the main area"; exit 1; }
  rx=$((rx + rw / 2))
  ry=$((ry + rh / 2))
  pointer_hold "$W" "$H" move "$rx" "$ry" press "$BTN_LEFT" move "$rx" $((ry - 4)) move "$rx" $((ry - 9)) \
    -- move $(((rx + tx) / 2)) $(((ry + ty) / 2)) mark mid hold move "$tx" $((ty + 6)) move "$tx" "$ty" \
    mark over hold release "$BTN_LEFT"
  "$ANDROMEDA" settle > /dev/null
  local claim mid over
  claim=$(probe "$scale" count "$RED")
  pointer_step mid
  "$ANDROMEDA" settle > /dev/null
  mid=$(probe "$scale" count "$RED")
  pointer_step over
  "$ANDROMEDA" settle > /dev/null
  over=$(probe "$scale" count "$RED")
  pointer_release
  "$ANDROMEDA" settle > /dev/null
  AREAS="$claim $mid $over"
}

check_shrinks() {
  local claim mid over
  read -r claim mid over <<< "$1"
  ((claim > mid && mid > over && over > 0)) ||
    { echo "$2: the dragged card did not shrink toward the thumb row: red px claim $claim, mid $mid, over $over"; exit 1; }
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 600 400 > /dev/null 2>&1 &
wait_title red
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" blue 600 400 > /dev/null 2>&1 &
wait_title blue

run_at_scale() {
  local scale=$1 home blue_ws bx by bw bh row_mid plus_x
  home=$(ws_of red)
  "$ANDROMEDA" msg "workspace-switch:$(index_of "$home")" > /dev/null
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle > /dev/null
  blue_ws=$(ws_of blue)
  read -r bx by bw bh < <(probe "$scale" bbox "$BLUE")
  ((bw > 0)) || { echo "scale $scale: no blue window in the thumb row"; exit 1; }
  row_mid=$((by + bh / 2))
  drag_red "$scale" $((bx + bw / 2)) $((by + bh / 2))
  [[ $(ws_of red) == "$blue_ws" ]] ||
    { echo "scale $scale: a drop on the $blue_ws thumb left red on $(ws_of red) (was $home)"; exit 1; }
  check_shrinks "$AREAS" "scale $scale thumb drop"
  on_screen red || { echo "scale $scale: the window dropped on a thumb left the screen: $("$ANDROMEDA" windows --json)"; exit 1; }
  echo "scale $scale: a drop on the thumb moved red from $home to $blue_ws; red px $AREAS"

  # Now red and blue share a desktop; the + thumb follows the last desktop's thumb in the row.
  local count before
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" msg "workspace-switch:$(index_of "$blue_ws")" > /dev/null
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle > /dev/null
  count=$("$ANDROMEDA" workspaces --json | jq 'length')
  plus_x=$(awk -v n="$count" -v w="$W" -v t="$THUMB_W" 'BEGIN { printf "%d", (w - (n + 1) * t - n * 18) / 2 + n * (t + 18) + t / 2 }')
  before=$(ws_of red)
  drag_red "$scale" "$plus_x" "$row_mid"
  [[ $(ws_of red) != "$before" && -n $(ws_of red) ]] ||
    { echo "scale $scale: a drop on + left red on $before"; "$ANDROMEDA" workspaces --json; exit 1; }
  check_shrinks "$AREAS" "scale $scale + drop"
  on_screen red || { echo "scale $scale: the window dropped on + left the screen: $("$ANDROMEDA" windows --json)"; exit 1; }
  echo "scale $scale: a drop on + moved red from $before to $(ws_of red); red px $AREAS"
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" settle > /dev/null
  # Back to two desktops for the next round: blue alone on one, red alone on another.
}

run_at_scale 1

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[output."HEADLESS-1"]
mode = "2560x1440"
scale = 2
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null
run_at_scale 2
echo "thumb and + drops move the window and the card shrinks toward the row at scale 1 and 2"
