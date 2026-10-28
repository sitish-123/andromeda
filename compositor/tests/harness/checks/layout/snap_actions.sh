#!/usr/bin/env bash
# Snap placement preserves the normal restore box through region changes, maximize, and fullscreen.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[[window_rule]]
match.title = "^snap-action$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" snap-action 480 300 > "$ANDROMEDA_RUNTIME_DIR/snap-action.log" 2>&1 &
title_bar_wait_box snap-action 480x300+173+109
snap() { "$ANDROMEDA" msg "window-snap:$1" > /dev/null; title_bar_wait_field snap-action snap "$1"; }
snap left
title_bar_wait_box snap-action 628x704+8+8
snap right
title_bar_wait_box snap-action 628x704+644+8
"$ANDROMEDA" msg window-snap:right > /dev/null
title_bar_wait_field snap-action snap null
title_bar_wait_box snap-action 480x300+173+109
snap bottom_right
title_bar_wait_box snap-action 628x348+644+364
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
title_bar_wait_box snap-action 1264x704+8+8
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
title_bar_wait_box snap-action 628x348+644+364
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
title_bar_wait_box snap-action 1280x720+0+0
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
title_bar_wait_field snap-action snap bottom_right
title_bar_wait_box snap-action 628x348+644+364
"$ANDROMEDA" msg window-unsnap > /dev/null
title_bar_wait_box snap-action 480x300+173+109
# Maximize-to-edges suppresses the title bar, but snapping must restore it before computing the region.
sed -i 's/mode = "off"/mode = "forced"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg window-toggle-maximize-to-edges > /dev/null
snap top_left
title_bar_wait_field snap-action title_bar true
title_bar_wait_box snap-action 628x320+8+36
# A manual size adjustment ends snapping instead of being refit on the next arrange.
"$ANDROMEDA" msg window-modify-width-right:0.1 > /dev/null
title_bar_wait_field snap-action snap null
# A fixed-size client larger than a quarter cannot snap.
FIXED_SIZE=1 "$ANDROMEDA_SUBSURFACE_CLIENT" fixed-snap 700 500 > "$ANDROMEDA_RUNTIME_DIR/fixed.log" 2>&1 &
title_bar_wait_field fixed-snap floating true
"$ANDROMEDA" msg "window-focus-warp:$(title_bar_window fixed-snap | jq -r .id)" > /dev/null
if reply=$("$ANDROMEDA" msg window-snap:top_left 2>&1); then
  echo "fixed-size client unexpectedly snapped"
  exit 1
fi
[[ $reply == *"cannot fit that region"* ]]
echo "snap actions preserve normal geometry and respect client constraints"

# A client can acknowledge the snap while retaining its old buffer dimensions. Fullscreen must remember the snap
# target, so its exit configures the snapped dimensions rather than adopting that stale buffer as the restore size.
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
HOLD_SIZE=1 LOG_CONFIGURES=1 "$ANDROMEDA_FRACTIONAL_CLIENT" pending-snap 480 300 > "$ANDROMEDA_RUNTIME_DIR/pending.log" 2>&1 &
title_bar_wait_field pending-snap floating true
"$ANDROMEDA" msg window-snap:left > /dev/null
await_lines "$ANDROMEDA_RUNTIME_DIR/pending.log" 'configure 628x676' 1
"$ANDROMEDA" settle > /dev/null
before=$(rg -c '^configure 628x676$' "$ANDROMEDA_RUNTIME_DIR/pending.log")
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
await_lines "$ANDROMEDA_RUNTIME_DIR/pending.log" 'configure 1280x720' 1
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
await_lines "$ANDROMEDA_RUNTIME_DIR/pending.log" 'configure 628x676' $((before + 1))
echo "fullscreen restore uses the snapped target while a client retains its old buffer"
