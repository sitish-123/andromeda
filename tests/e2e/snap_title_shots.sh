#!/usr/bin/env bash
# Captures the compositor chrome added for Free mode and Desktop Layouts, for design review against
# .agent/mockups/overview-proto: title bars in the andromeda, macos and windows styles, the Snap Layouts flyout, and
# snap assist. Usage: tests/e2e/snap_title_shots.sh; images in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/snap-title-shots}
source "$(dirname "$0")/lib.sh"
UNMAP=$ROOT/compositor/build-debug/tests/unmap-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

boot_headless 1
cat >> "$RUNTIME/andromeda.toml" <<'CONFIG'

[layout]
mode = "floating"
[appearance.title_bar]
mode = "forced"
style = "andromeda"
button_position = "right"
[[window_rule]]
match.title = "^Mail$"
default_position = { x = 120, y = 110, anchor = "top_left" }
[[window_rule]]
match.title = "^Notes$"
default_position = { x = 620, y = 260, anchor = "top_left" }
CONFIG
run "$ANDROMEDA" msg config-reload > /dev/null

export UNMAP POINTER
with_noctalia '
  count() { "$ANDROMEDA" windows --json | jq length; }
  FILL_COLOR=0xFFE8E8EE "$UNMAP" Notes 520 340 > "$OUT/notes.log" 2>&1 &
  FILL_COLOR=0xFFF4F4F8 "$UNMAP" Mail 560 360 > "$OUT/mail.log" 2>&1 &
  for _ in $(seq 100); do [[ $(count) -ge 2 ]] && break; sleep 0.05; done
  "$ANDROMEDA" settle > /dev/null
  conf="$RUNTIME/andromeda.toml"
  for style in andromeda macos windows; do
    side=right; [[ $style == macos ]] && side=left
    sed -i "s/^style = .*/style = \"$style\"/; s/^button_position = .*/button_position = \"$side\"/" "$conf"
    "$ANDROMEDA" msg config-reload > /dev/null
    "$ANDROMEDA" settle > /dev/null
    grim -g "100,70 620x70" "$OUT/title-$style.png"
  done
  # The Windows cell fill on hover: the pointer on the Mail close cell.
  "$POINTER" 1280 720 move 668 96 > /dev/null
  "$ANDROMEDA" settle > /dev/null
  grim -g "100,70 620x70" "$OUT/title-windows-hover.png"
  sed -i "s/^style = .*/style = \"andromeda\"/; s/^button_position = .*/button_position = \"right\"/" "$conf"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" msg window-snap-layouts > /dev/null
  "$ANDROMEDA" settle > /dev/null
  grim "$OUT/flyout.png"
  "$POINTER" 1280 720 tap 28 > /dev/null
  "$ANDROMEDA" settle > /dev/null
  grim "$OUT/assist.png"
'
ls "$OUT"/*.png
