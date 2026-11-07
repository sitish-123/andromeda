#!/usr/bin/env bash
# Strip shows one tab per workspace and a + tab in the dock's place (mockup B): + adds a workspace and goes there, the
# first tab goes back to workspace 1, with the overview open.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
IMAGE="$ANDROMEDA_RUNTIME_DIR/tabs.png"
# The tab bar: 32 px high, 24 px above the usable bottom (the whole output here).
readonly BAND="${OUTPUT_W}x32+0+$((OUTPUT_H - 24 - 32))"

pointer() { "$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[overview]
style = "strip"
background_blur = false

[animation]
enabled = false
EOF
"$ANDROMEDA" msg config-reload > /dev/null

active() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .index'; }
count() { "$ANDROMEDA" workspaces --json | jq 'length'; }
wait_until() {
  for _ in $(seq 60); do
    eval "$1" && return 0
    sleep 0.05
  done
  echo "$2: $("$ANDROMEDA" workspaces --json)"
  return 1
}
# "left right y" of the tab text in the bar.
tabs() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.55 && g > 0.55 && b > 0.55' "$BAND")
  ((w > 0)) || { echo "no tab text in the bar band" >&2; return 1; }
  echo "$x $((x + w - 1)) $((y + h / 2))"
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" tabs-red 600 400 > /dev/null 2>&1 &
wait_until '[[ $("$ANDROMEDA" windows --json | jq length) -eq 1 ]]' "the window did not map"
"$ANDROMEDA" settle
first=$(active)
before=$(count)
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

read -r left right y < <(tabs)
pointer move "$((right - 3))" "$y" click "$BTN_LEFT"
wait_until '[[ $(active) != "$first" ]]' "the + tab did not go to a new workspace"
"$ANDROMEDA" settle
read -r left right y < <(tabs)
pointer move "$((left + 4))" "$y" click "$BTN_LEFT"
wait_until '[[ $(active) == "$first" ]]' "the first tab did not go back to workspace $first"
echo "Strip tabs: + left workspace $first for a new one ($before workspaces before), the first tab came back"
