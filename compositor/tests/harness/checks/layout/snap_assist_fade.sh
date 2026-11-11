#!/usr/bin/env bash
# Snap assist fades out its last picture after Escape. A window shown as a card that closes during the fade must not
# reach the freed card, and a workspace switch during the fade hides assist at once, as it does an open assist.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
readonly K_ENTER=28 K_ESC=1
readonly SHOT_BEFORE="$ANDROMEDA_RUNTIME_DIR/assist-fade-before.png" SHOT_AFTER="$ANDROMEDA_RUNTIME_DIR/assist-fade-after.png"
# The right half less a margin for fade-a's shadow; all windows stay under fade-a, so only assist draws here.
readonly ZONE=600x680+660+20
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[appearance]
border_width = 0
outer_border_width = 0
reduce_motion = true
[appearance.title_bar]
mode = "forced"
height = 28
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^fade-[abc]$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
"$ANDROMEDA_UNMAP_CLIENT" fade-c 300 200 > "$ANDROMEDA_RUNTIME_DIR/fade-c.log" 2>&1 &
title_bar_wait_box fade-c 300x200+100+100
"$ANDROMEDA_FRACTIONAL_CLIENT" fade-b 400 300 > "$ANDROMEDA_RUNTIME_DIR/fade-b.log" 2>&1 &
title_bar_wait_box fade-b 400x300+100+100
"$ANDROMEDA_FRACTIONAL_CLIENT" fade-a 400 300 > "$ANDROMEDA_RUNTIME_DIR/fade-a.log" 2>&1 &
title_bar_wait_box fade-a 400x300+100+100
pointer() { "$ANDROMEDA_POINTER_CLIENT" 1280 720 "$@"; }
alive() {
  "$ANDROMEDA" windows --json > /dev/null 2>&1 || { echo "the compositor died after $1"; exit 1; }
}
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$SHOT_BEFORE"

# Mod+Z, Enter: fade-a snaps to the left half and assist offers fade-b and fade-c in the right half.
"$ANDROMEDA" msg window-snap-layouts > /dev/null
pointer tap "$K_ENTER"
title_bar_wait_field fade-a snap 1.1
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" clock-advance 3000

pointer tap "$K_ESC"
"$ANDROMEDA" clock-advance 16
"$ANDROMEDA" msg "window-close:$(title_bar_window fade-c | jq -r .id)" > /dev/null
"$ANDROMEDA" clock-advance 16
alive "closing a card's window during the fade"

# The frozen clock holds the workspace switch at its first frame, so the zone shows workspace 1 without assist.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" clock-advance 1
grim -o HEADLESS-1 "$SHOT_AFTER"
diff=$("$ANDROMEDA_PIXEL_PROBE" "$SHOT_AFTER" diff "$SHOT_BEFORE" "$ZONE")
"$ANDROMEDA" clock-advance 3000
alive "the fade ran out on another workspace"
[[ $diff == "0 0" ]] || { echo "the fading assist still showed after a workspace switch: $diff (max, px)"; exit 1; }
echo "a card's window closed during the fade, and a workspace switch hid the fading assist at once"
