#!/usr/bin/env bash
# harness: outputs=2
# Both ordinary moves and workspace swaps preserve the reason a window floats.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "forced"
[[window_rule]]
match.title = "^explicit$"
default_floating = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
spawn() {
  "$ANDROMEDA_FRACTIONAL_CLIENT" "$1" 480 300 > "$ANDROMEDA_RUNTIME_DIR/$1.log" 2>&1 &
  title_bar_wait_field "$1" floating true
}
focus() { "$ANDROMEDA" msg "window-focus-warp:$(title_bar_window "$1" | jq -r .id)" > /dev/null; }
spawn policy
spawn explicit
focus policy
"$ANDROMEDA" msg window-move-to-workspace:2 > /dev/null
title_bar_wait_field policy floating false
"$ANDROMEDA" msg window-move-to-workspace:1 > /dev/null
title_bar_wait_field policy floating true
# A whole-column transfer uses the same floating destination policy.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" tiled 480 300 > "$ANDROMEDA_RUNTIME_DIR/tiled.log" 2>&1 &
title_bar_wait_field tiled floating false
"$ANDROMEDA" msg column-move-to-workspace:1 > /dev/null
title_bar_wait_field tiled floating true
# Swapping layouts' contents must retile policy floats but keep explicit floats.
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
focus policy
"$ANDROMEDA" msg workspace-swap-active-output-next > /dev/null
title_bar_wait_field policy floating false
title_bar_wait_field tiled floating false
title_bar_wait_field explicit floating true
"$ANDROMEDA" msg workspace-swap-active-output-next > /dev/null
title_bar_wait_field policy floating true
title_bar_wait_field tiled floating true
title_bar_wait_field explicit floating true
"$ANDROMEDA" msg workspace-set-layout:dwindle > /dev/null
title_bar_wait_field policy floating false
title_bar_wait_field tiled floating false
title_bar_wait_field explicit floating true
echo "floating layout workspace and column transfers verified"
