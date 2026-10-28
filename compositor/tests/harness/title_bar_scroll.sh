# Shared real-client repro for a title bar press during a scrolling focus reveal.
source "$ANDROMEDA_HARNESS_LIB"
mode=$1
cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[layout]
mode = "scrolling"
[layout.scrolling]
default_extent_fraction = 0.666667
[animation.windows_in]
enabled = false
[animation.windows_move]
duration_ms = 1000
curve = "linear"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "$mode"
height = 28
[input.focus]
follows_mouse = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

wait_count() {
  for _ in $(seq 120); do
    [[ $("$ANDROMEDA" windows --json | jq length) == "$1" ]] && return 0
    sleep 0.025
  done
  echo "expected $1 windows: $("$ANDROMEDA" windows --json)"
  return 1
}

assert_gap() {
  local image=$ANDROMEDA_RUNTIME_DIR/$1.png rx ry rw rh bx by bw bh
  grim "$image"
  read -r rx ry rw rh < <("$ANDROMEDA_PIXEL_PROBE" "$image" bbox 'r > 0.8 && g < 0.2 && b < 0.2')
  read -r bx by bw bh < <("$ANDROMEDA_PIXEL_PROBE" "$image" bbox 'b > 0.7 && r < 0.3 && g > 0.4')
  if ((rw <= 0 || bw <= 0 || bx - rx - rw != 8 || ry != by || rh != bh)); then
    echo "$1: painted tiles separated: red=$rx,$ry ${rw}x$rh blue=$bx,$by ${bw}x$bh"
    return 1
  fi
  BLUE_X=$bx
}

FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" gap-left 400 300 \
  > "$ANDROMEDA_RUNTIME_DIR/left.log" 2>&1 &
wait_count 1
left=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "gap-left") | .id')

for phase in held released; do
  fifo=$ANDROMEDA_RUNTIME_DIR/redraw-$phase.fifo
  log=$ANDROMEDA_RUNTIME_DIR/right-$phase.log
  mkfifo "$fifo"
  REDRAW_CONTROL=1 "$ANDROMEDA_SEAT_LOG_CLIENT" gap-right move-on-press < "$fifo" > "$log" 2>&1 &
  client=$!
  exec {redraw_fd}> "$fifo"
  wait_count 2
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg "window-focus:$left" > /dev/null
  "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" clock-freeze > /dev/null
  assert_gap "$phase-before"
  start_x=$BLUE_X
  windows=$("$ANDROMEDA" windows --json)
  x=$(jq -r '.[] | select(.title == "gap-right") | .x + 80' <<< "$windows")
  y=$(jq -r '.[] | select(.title == "gap-right") | .y' <<< "$windows")
  if [[ $mode == forced ]]; then
    y=$((y - 14))
  else
    y=$((y + 14))
  fi
  pointer_hold 1280 720 move "$x" "$y" press 272 -- release 272
  if [[ $mode == off ]]; then
    await_lines "$log" move-requested 1 0.025
  fi
  "$ANDROMEDA" clock-advance 250 > /dev/null
  assert_gap "$phase-before-redraw"
  before_redraw=$BLUE_X
  ((before_redraw < start_x)) || { echo "press did not start a focus reveal"; exit 1; }

  # Commit a normal application repaint while its title bar move is still below the drag threshold.
  # Misclassifying this pending move as a resize cancels only this tile's layout motion on commit.
  echo r >&"$redraw_fd"
  await_lines "$log" redrawn 1 0.025
  if [[ $phase == released ]]; then
    pointer_release
  fi
  "$ANDROMEDA" clock-advance 250 > /dev/null
  assert_gap "$phase-after-redraw"
  ((BLUE_X < before_redraw)) || { echo "repaint stopped the clicked tile's reveal"; exit 1; }
  "$ANDROMEDA" clock-advance 1000 > /dev/null
  assert_gap "$phase-complete"
  target_x=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "gap-right") | .x')
  [[ $BLUE_X == "$target_x" ]] || { echo "clicked tile stopped at $BLUE_X instead of $target_x"; exit 1; }
  if [[ $phase == held ]]; then
    pointer_release
  fi
  "$ANDROMEDA" settle > /dev/null
  assert_gap "$phase-after-release"
  "$ANDROMEDA" clock-resume > /dev/null
  kill "$client"
  wait "$client" 2>/dev/null || true
  exec {redraw_fd}>&-
  wait_count 1
done
echo "title bar presses retained tiled spacing through repaint, held reveal, and early release ($mode)"
