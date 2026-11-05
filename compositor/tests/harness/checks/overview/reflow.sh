#!/usr/bin/env bash
# Closing a window inside the overview reflows the survivors: a Spaces exposé of two free windows becomes one, and the
# survivor's card glides to its new place on the overview curve instead of jumping there.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
IMAGE="$ANDROMEDA_RUNTIME_DIR/reflow.png"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "floating"

[overview]
style = "spaces"
background_blur = false
shortcuts = false

[animation.overview]
duration_ms = 1000
curve = "linear"
workspace_curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.05
  done
  echo "expected $1 window(s): $("$ANDROMEDA" windows --json)"
  return 1
}

# Center x of the blue survivor's card.
blue_center() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r x _ w _ < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
  ((w > 0)) || { echo "the survivor's card is not on screen" >&2; return 1; }
  echo $((x + w / 2))
}

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" reflow-closing 500 400 > "$ANDROMEDA_RUNTIME_DIR/closing.log" 2>&1 &
wait_for_windows 1
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" reflow-survivor 500 400 > /dev/null 2>&1 &
wait_for_windows 2

"$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 5 5
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
before=$(blue_center)

"$ANDROMEDA" clock-freeze > /dev/null
closing=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "reflow-closing") | .id')
"$ANDROMEDA" msg "window-close:$closing" > /dev/null
wait_for_windows 1
"$ANDROMEDA" clock-advance 500 > /dev/null
middle=$(blue_center)
"$ANDROMEDA" clock-advance 1500 > /dev/null
after=$(blue_center)
"$ANDROMEDA" clock-resume > /dev/null

travel=$((after - before))
((${travel#-} > 40)) || { echo "the survivor did not reflow: before=$before after=$after"; exit 1; }
# Linear over 1000 ms: half way at 500 ms, with slack for whole-pixel rounding.
progress=$(((middle - before) * 100 / travel))
((progress > 25 && progress < 75)) || {
  echo "the survivor jumped instead of gliding: before=$before middle=$middle after=$after"
  exit 1
}
echo "a closed window's neighbour glides to its new exposé place: $before -> $middle -> $after"
