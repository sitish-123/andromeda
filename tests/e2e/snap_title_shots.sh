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
  # The Mail title bar (40 high) with 8 px around it, from where the window is now.
  title_box() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | \"\(.x - 8),\(.y - 48) \(.w + 16)x56\""; }
  FILL_COLOR=0xFFE8E8EE "$UNMAP" Notes 520 340 > "$OUT/notes.log" 2>&1 &
  FILL_COLOR=0xFFF4F4F8 "$UNMAP" Mail 560 360 > "$OUT/mail.log" 2>&1 &
  for _ in $(seq 100); do [[ $(count) -ge 2 ]] && break; sleep 0.05; done
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  conf="$RUNTIME/andromeda.toml"
  for style in andromeda macos windows; do
    side=right; [[ $style == macos ]] && side=left
    sed -i "s/^style = .*/style = \"$style\"/; s/^button_position = .*/button_position = \"$side\"/" "$conf"
    "$ANDROMEDA" msg config-reload > /dev/null
    "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
    grim -g "$(title_box)" "$OUT/title-$style.png"
    if [[ $style == macos ]]; then
      # Sequoia shows the glyphs while the pointer is over the traffic lights (12 round, inset 12).
      lights=$("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | \"\(.x + 18) \(.y - 20)\"")
      "$POINTER" 1280 720 move $lights > /dev/null
      "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
      grim -g "$(title_box)" "$OUT/title-macos-hover.png"
      "$POINTER" 1280 720 move 640 700 > /dev/null
    fi
    "$NOCTALIA" msg theme-mode-set dark > /dev/null
    "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
    grim -g "$(title_box)" "$OUT/title-$style-dark.png"
    "$NOCTALIA" msg theme-mode-set light > /dev/null
    "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  done
  # The Windows cell fill on hover: the pointer on the Mail close cell.
  close=$("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | \"\(.x + .w - 23) \(.y - 20)\"")
  "$POINTER" 1280 720 move $close > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$(title_box)" "$OUT/title-windows-hover.png"
  # The Snap Layouts flyout from a rest on the maximize cell (the 46 px cell left of close).
  max=$("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | \"\(.x + .w - 69) \(.y - 20)\"")
  "$POINTER" 1280 720 move $max > /dev/null
  sleep 0.6 # real time: the 400 ms maximize dwell timer
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/flyout-hover.png"
  "$POINTER" 1280 720 move 640 700 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # The top-edge plate: Mail dragged by its title to the top edge and held, then put back.
  grab=$("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | \"\(.x + (.w / 2 | floor)) \(.y - 20)\"")
  read -r gx gy <<< "$grab"
  "$POINTER" 1280 720 move $gx $gy press 272 move $gx $((gy - 40)) move 640 1 > /dev/null
  sleep 0.3 # real time: the 150 ms snap dwell timer
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/drag-plate.png"
  "$POINTER" 1280 720 move $gx $((gy - 40)) move-release $gx $gy 272 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  sed -i "s/^style = .*/style = \"andromeda\"/; s/^button_position = .*/button_position = \"right\"/" "$conf"
  "$ANDROMEDA" msg config-reload > /dev/null
  # Five assist candidates: Notes and four more windows.
  for t in Photos Music Terminal Code; do
    FILL_COLOR=0xFFDCDCE4 "$UNMAP" $t 420 300 > "$OUT/$t.log" 2>&1 &
  done
  for _ in $(seq 100); do [[ $(count) -ge 6 ]] && break; sleep 0.05; done
  "$ANDROMEDA" msg window-focus:"$("$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"Mail\") | .id")" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "$ANDROMEDA" msg window-snap-layouts > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/flyout.png"
  "$POINTER" 1280 720 tap 28 > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/assist.png"
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/assist-dark.png"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
'
ls "$OUT"/*.png
