#!/usr/bin/env bash
# Spaces overview (thumb row above): a window card dropped on another desktop's thumb moves the window to that desktop,
# and one dropped on the + thumb moves it to a new desktop. The dragged card shrinks as the pointer nears the thumb row
# and ends inside the thumb. Escape drops a held card back into its slot and keeps the overview open. All at output
# scale 1, 1.5 and 2. drag_to_thumb_taskview.sh runs it in Task View (row below) with a floating window, which must
# stay on screen. drag_to_thumb_animated.sh runs it with animations on, where a second card cancelled while the first
# still returns must not snap the first into its slot.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272
readonly W=1280 H=720
readonly STYLE=${DRAG_TO_THUMB_STYLE:-spaces}
readonly FLOATING=${DRAG_TO_THUMB_FLOATING:-false}
readonly ANIMATED=${DRAG_TO_THUMB_ANIMATED:-false}
readonly KEY_ESC=1
# Thumb width in the row (style.cpp kSpacesThumbWidth, kTaskViewThumbWidth).
THUMB_W=176
[[ $STYLE == taskview ]] && THUMB_W=208
IMAGE="$ANDROMEDA_RUNTIME_DIR/drag-to-thumb.png"
readonly RED='r > 0.6 && g < 0.3 && b < 0.3'
readonly BLUE='b > 0.6 && r < 0.3 && g < 0.3'

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[animation]
enabled = $ANIMATED
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

# inside_thumb <scale> <workspace count> <row middle y>: the red card lies in one thumb of the row, the blue one's.
inside_thumb() {
  local scale=$1 n=$2 row_mid=$3 rx ry rw rh bx by bw bh left th
  read -r rx ry rw rh < <(probe "$scale" bbox "$RED")
  read -r bx by bw bh < <(probe "$scale" bbox "$BLUE")
  ((rw > 0 && bw > 0)) || { echo "scale $scale: red ${rw}x${rh}, blue ${bw}x${bh}: a card is missing after the drop"; exit 1; }
  # The thumb holding the blue card, from the row's centred layout (the + drop below uses the same).
  left=$(awk -v n="$n" -v w="$W" -v t="$THUMB_W" -v c=$((bx + bw / 2)) 'BEGIN {
    l = (w - (n + 1) * t - n * 18) / 2; printf "%d", l + int((c - l) / (t + 18)) * (t + 18) }')
  th=$((THUMB_W * H / W))
  ((rx >= left - 1 && rx + rw <= left + THUMB_W + 1 && ry >= row_mid - th && ry + rh <= row_mid + th)) || {
    echo "scale $scale: the dropped card ended at ${rw}x${rh}+${rx}+${ry}, not in the thumb at x $left..$((left + THUMB_W))"
    exit 1
  }
}

# escape_card <predicate> <scale>: holds that card 100 px below its slot and presses Escape there.
escape_card() {
  local predicate=$1 scale=$2 x y w h
  read -r x y w h < <(probe "$scale" bbox "$predicate")
  x=$((x + w / 2))
  y=$((y + h / 2))
  "$ANDROMEDA_POINTER_CLIENT" "$W" "$H" move "$x" "$y" press "$BTN_LEFT" move "$x" $((y + 4)) move "$x" $((y + 9)) \
    move "$x" $((y + 100)) tap "$KEY_ESC" release "$BTN_LEFT"
}

# return_check <scale>: Escape returns the held red card to its slot and the overview stays open (the slot is an
# overview card). Animated, blue cancelled while red still returns leaves red on its own path, sampled over frames.
return_check() {
  local scale=$1 x0 y0 w0 h0 x1 y1 x2 y2 x3 y3 x y w h
  read -r x0 y0 w0 h0 < <(probe "$scale" bbox "$RED")
  ((w0 > 0 && w0 < 600)) || { echo "scale $scale: no red overview card before Escape (${w0}x${h0})"; exit 1; }
  if [[ $ANIMATED == true ]]; then
    "$ANDROMEDA" clock-freeze > /dev/null
    escape_card "$RED" "$scale"
    "$ANDROMEDA" clock-advance 48 > /dev/null
    read -r x1 y1 _ _ < <(probe "$scale" bbox "$RED")
    ((y1 > y0 + 8)) || { "$ANDROMEDA" clock-resume > /dev/null; echo "scale $scale: red was not returning (y $y0 -> $y1)"; exit 1; }
    escape_card "$BLUE" "$scale"
    read -r x2 y2 _ _ < <(probe "$scale" bbox "$RED")
    "$ANDROMEDA" clock-advance 16 > /dev/null
    read -r x3 y3 _ _ < <(probe "$scale" bbox "$RED")
    "$ANDROMEDA" clock-resume > /dev/null
    ((x2 - x1 <= 2 && x1 - x2 <= 2 && y2 - y1 <= 2 && y1 - y2 <= 2)) || {
      echo "scale $scale: a second return moved the returning red card from +$x1+$y1 to +$x2+$y2 at once (slot +$x0+$y0)"
      exit 1
    }
    ((y3 < y2 && y3 > y0)) || { echo "scale $scale: red stopped returning after the second drop: y $y0, $y1, $y2, $y3"; exit 1; }
    echo "scale $scale: red keeps returning while blue returns: y $y1 -> $y2 -> $y3 -> slot $y0"
  else
    escape_card "$RED" "$scale"
  fi
  "$ANDROMEDA" settle > /dev/null
  read -r x y w h < <(probe "$scale" bbox "$RED")
  ((x - x0 <= 1 && x0 - x <= 1 && y - y0 <= 1 && y0 - y <= 1 && w - w0 <= 1 && w0 - w <= 1)) || {
    echo "scale $scale: after Escape red is at ${w}x${h}+${x}+${y}, not in its slot ${w0}x${h0}+${x0}+${y0}"
    exit 1
  }
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
  inside_thumb "$scale" "$("$ANDROMEDA" workspaces --json | jq 'length')" "$row_mid"
  on_screen red || { echo "scale $scale: the window dropped on a thumb left the screen: $("$ANDROMEDA" windows --json)"; exit 1; }
  echo "scale $scale: a drop on the thumb moved red from $home to $blue_ws; red px $AREAS"

  # Now red and blue share a desktop; the + thumb follows the last desktop's thumb in the row.
  local count before
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" msg "workspace-switch:$(index_of "$blue_ws")" > /dev/null
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle > /dev/null
  return_check "$scale"
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

at_scale() {
  sed -i '/^\[output\."HEADLESS-1"\]$/,+2d' "$ANDROMEDA_CONFIG"
  printf '[output."HEADLESS-1"]\nmode = "%s"\nscale = %s\n' "$2" "$1" >> "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" settle > /dev/null
  run_at_scale "$1"
}

run_at_scale 1
# Settling every spring makes each round slow; scale 1 covers the animated returns.
if [[ $ANIMATED == true ]]; then
  echo "animated: thumb and + drops land in place, Escape returns the card, a second return leaves the first on its path"
  exit 0
fi
at_scale 1.5 1920x1080
at_scale 2 2560x1440
echo "thumb and + drops move the window, the card shrinks toward the row and lands in the thumb, Escape returns it, at scale 1, 1.5 and 2"
