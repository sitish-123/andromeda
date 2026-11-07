#!/usr/bin/env bash
# Right-clicking a window card opens the window menu (DESIGN.md "Overview v3" Completion): its first row moves the
# window to the next desktop with the overview still open, and Up, Up, Enter picks Close.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272 BTN_RIGHT=273
readonly K_UP=103 K_ENTER=28
IMAGE="$ANDROMEDA_RUNTIME_DIR/menu.png"

pointer() { "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "floating"

[overview]
style = "spaces"
background_blur = false

[animation]
enabled = false
EOF
"$ANDROMEDA" msg config-reload > /dev/null

wait_for() {
  for _ in $(seq 60); do
    eval "$1" && return 0
    sleep 0.05
  done
  echo "$2: $("$ANDROMEDA" windows --json)"
  return 1
}
workspace_of() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"$1\") | .workspace"; }
count() { "$ANDROMEDA" windows --json | jq 'length'; }
centre() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$1")
  ((w > 0)) || { echo "no card for '$1' on screen" >&2; return 1; }
  echo "$((x + w / 2)) $((y + h / 2))"
}

FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" menu-blue 500 400 > /dev/null 2>&1 &
wait_for '[[ $(count) -eq 1 ]]' "the blue window did not map"
FILL_COLOR=0xFF00FF00 "$ANDROMEDA_UNMAP_CLIENT" menu-green 500 400 > /dev/null 2>&1 &
wait_for '[[ $(count) -eq 2 ]]' "the green window did not map"
"$ANDROMEDA" settle
start=$(workspace_of menu-blue)
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

# The menu opens at the pointer; its first row is 6 px of padding plus half a 32 px row below it.
read -r bx by < <(centre 'b > 0.9 && r < 0.1 && g < 0.1')
pointer move "$bx" "$by" click "$BTN_RIGHT"
"$ANDROMEDA" settle
pointer move "$((bx + 40))" "$((by + 22))" click "$BTN_LEFT"
wait_for '[[ $(workspace_of menu-blue) != "$start" ]]' "Move to on the first row did not move the blue window"

"$ANDROMEDA" settle
read -r gx gy < <(centre 'g > 0.9 && r < 0.1 && b < 0.1')
pointer move "$gx" "$gy" click "$BTN_RIGHT" tap "$K_UP" tap "$K_UP" tap "$K_ENTER"
wait_for '[[ $(count) -eq 1 ]]' "Up, Up, Enter on the window menu did not close the green window"
echo "the window menu moves a window to another desktop and closes one from the keyboard"
