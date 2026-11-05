#!/usr/bin/env bash
# The overview labels its windows: with overview.titles = "always" every window card carries a title capsule under it,
# with "hover" only the selected one does, and closing the overview takes the chrome away with it.
set -euo pipefail

IMAGE="$ANDROMEDA_RUNTIME_DIR/chrome.png"

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "floating"

[overview]
style = "spaces"
titles = "always"
background_blur = false
workspace_wallpaper = false
shortcuts = false

[animation.overview]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.05
  done
  echo "expected $1 window(s): $("$ANDROMEDA" windows --json)"
  return 1
}

FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" chrome-blue 500 400 > /dev/null 2>&1 &
wait_for_windows 1
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" chrome-red 500 400 > /dev/null 2>&1 &
wait_for_windows 2
"$ANDROMEDA" settle

# Light text pixels in the band just under the blue (unselected) card.
label_pixels() {
  grim -o HEADLESS-1 "$IMAGE"
  local x y w h
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
  if ((w == 0)); then
    echo "the blue window's card is not on screen" >&2
    return 1
  fi
  "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count 'r > 0.7 && g > 0.7 && b > 0.7' "${w}x30+${x}+$((y + h))"
}

open_overview() {
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle
}
close_overview() {
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" settle
}

open_overview
always=$(label_pixels)
if ((always < 20)); then
  echo "titles = always: the unselected card has no title capsule under it ($always light pixels)"
  exit 1
fi
close_overview
grim -o HEADLESS-1 "$IMAGE"
closed=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count 'r > 0.7 && g > 0.7 && b > 0.7')
if ((closed > 0)); then
  echo "closing the overview left $closed light chrome pixels on screen"
  exit 1
fi

sed -i 's/^titles = "always"$/titles = "hover"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
open_overview
hover=$(label_pixels)
if ((hover > 0)); then
  echo "titles = hover: the unselected card shows a title capsule ($hover light pixels)"
  exit 1
fi

echo "title capsules follow overview.titles (always: $always light pixels, hover: none) and leave with the overview"
