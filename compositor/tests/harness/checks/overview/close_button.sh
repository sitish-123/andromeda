#!/usr/bin/env bash
# Hovering a window card in the overview shows a close button at its top-right corner; it turns red under the
# pointer, a click on it closes that window, and sliding off before letting go cancels the close.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
IMAGE="$ANDROMEDA_RUNTIME_DIR/close_button.png"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "floating"

[overview]
style = "spaces"
titles = "hover"
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

FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" close-blue 500 400 > /dev/null 2>&1 &
wait_for_windows 1
FILL_COLOR=0xFF00FF00 "$ANDROMEDA_UNMAP_CLIENT" close-green 500 400 > /dev/null 2>&1 &
wait_for_windows 2
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

grim -o HEADLESS-1 "$IMAGE"
read -r bx by bw bh < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
if ((bw == 0)); then
  echo "the blue window's card is not on screen"
  exit 1
fi
# The button is 26 px, centred 4 px inside the card's top-right corner.
cx=$((bx + bw - 4))
cy=$((by + 4))
button="26x26+$((cx - 13))+$((cy - 13))"
light='r > 0.7 && g > 0.7 && b > 0.7'
red='r > 0.8 && g > 0.18 && g < 0.4 && b > 0.18 && b < 0.42'

idle=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$light" "$button")
if ((idle > 0)); then
  echo "a close button shows on the blue card before the pointer is over it ($idle light pixels)"
  exit 1
fi

pointer move $((bx + bw / 2)) $((by + bh / 2))
"$ANDROMEDA" settle
grim -o HEADLESS-1 "$IMAGE"
hovered=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$light" "$button")
if ((hovered < 5)); then
  echo "hovering the blue card shows no close button at its top-right corner ($hovered light pixels)"
  exit 1
fi

pointer move "$cx" "$cy"
"$ANDROMEDA" settle
grim -o HEADLESS-1 "$IMAGE"
hot=$("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" count "$red" "$button")
if ((hot < 100)); then
  echo "the close button does not turn red under the pointer ($hot red pixels)"
  exit 1
fi

# Press, slide off, release: nothing closes.
pointer press "$BTN_LEFT" move $((bx + bw / 2)) $((by + bh / 2)) release "$BTN_LEFT"
"$ANDROMEDA" settle
if [[ $("$ANDROMEDA" windows --json | jq 'length') -ne 2 ]]; then
  echo "sliding off the close button before releasing still closed a window"
  exit 1
fi
grim -o HEADLESS-1 "$IMAGE"
read -r _ _ still _ < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
if ((still != bw)); then
  echo "releasing off the close button left the overview (blue card ${bw} px wide, now ${still} px)"
  exit 1
fi

pointer move "$cx" "$cy" click "$BTN_LEFT"
wait_for_windows 1
left=$("$ANDROMEDA" windows --json | jq -r '.[0].title')
if [[ $left != close-green ]]; then
  echo "the close button closed $left instead of the blue window"
  exit 1
fi

echo "hovering a card shows its close button ($hovered light pixels), red under the pointer ($hot), and a click closes it"
