#!/usr/bin/env bash
# harness: outputs=1
# A float that sat fully inside the usable area stays fully inside when a panel reserves space after it mapped, as a
# bar does at session start.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"

cat >> "$ANDROMEDA_CONFIG" << 'EOF2'

[animation]
enabled = false

[[window_rule]]
match.title = "^inside-"
default_floating = true
default_floating_size = { width = 0.5, height = 0.9 }
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

field_of() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

"$CLIENT" inside-float > "$ANDROMEDA_RUNTIME_DIR/inside-float.log" 2>&1 &
for _ in $(seq 80); do
  [[ $(field_of inside-float h) == 648 ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
y0=$(field_of inside-float y)
((y0 < 40)) || { echo "the float opened at y=$y0, not under the panel's future zone"; exit 1; }

"$LAYER_CLIENT" HEADLESS-1 40 > "$ANDROMEDA_RUNTIME_DIR/panel.log" 2>&1 &
y=
for _ in $(seq 80); do
  y=$(field_of inside-float y)
  ((y >= 40)) && break
  sleep 0.05
done
((y >= 40)) || { echo "the float stayed at y=$y under the 40 px panel"; exit 1; }
echo "the float moved from y=$y0 to y=$y below the panel"
