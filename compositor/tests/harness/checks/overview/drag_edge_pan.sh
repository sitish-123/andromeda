#!/usr/bin/env bash
# Strip overview, Scrolling workspace: holding a dragged window at the left edge pans the row continuously, so the
# other columns move right while the drag is held.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[overview]
style = "strip"
zoom = 0.5
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
titles=(one two three four)
for title in "${titles[@]}"; do
  "$ANDROMEDA_FRACTIONAL_CLIENT" "$title" 480 300 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
  title_bar_wait_field "$title" floating false
done
"$ANDROMEDA" settle > /dev/null
xs() { for title in "${titles[@]}"; do title_bar_window "$title" | jq -r '.x'; done; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# The focused last column sits at the right end of the row, so a left pan has room. Strip at zoom 0.5 maps a
# window's centre to (320 + x / 2, 180 + y / 2); the drag moves into the 72 px left band and holds there.
read -r x y < <(title_bar_window four | jq -r '"\(320 + (.x + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
pointer_hold 1280 720 move "$x" "$y" press 272 move "$x" $((y + 20)) move 200 "$y" move 20 "$y" -- release 272
mapfile -t before < <(xs)
moved=0
for _ in $(seq 60); do
  mapfile -t during < <(xs)
  moved=0
  for i in "${!titles[@]}"; do
    ((during[i] > before[i])) && moved=$((moved + 1))
  done
  ((moved >= 2)) && break
  sleep 0.05
done
pointer_release
((moved >= 2)) || { echo "holding at the left edge did not pan: before ${before[*]}, during ${during[*]}"; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
echo "holding a drag at the left edge panned $moved columns right"
