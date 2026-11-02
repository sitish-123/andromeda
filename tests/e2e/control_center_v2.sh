#!/usr/bin/env bash
# Control center v2 flow (rulebook 9.16): the Dark Mode tile toggles the theme, the Wi-Fi chevron opens the detail page and
# back returns, a page dot changes the page, edit mode removes a tile and Done persists [control_center] quick_tiles.
# Every step waits on state (theme value, a changed panel crop, the saved file) with a timeout. Shots go to $OUT
# (default ./artifacts/control-center-v2).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/control-center-v2}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  # wait_for SECONDS COMMAND...: poll COMMAND every 0.2 s.
  wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }
  theme_is() { [[ $("$NOCTALIA" msg theme-mode-get 2> /dev/null) == *"$1"* ]]; }
  # The sheet crop (below the bar clock) differs from the last shot kept in $OUT/last.png.
  panel_changed() {
    grim "$OUT/now.png" && python3 -I - "$OUT/last.png" "$OUT/now.png" <<PY
import sys
from PIL import Image, ImageChops
box = (880, 60, 1280, 500)
a, b = (Image.open(p).convert("RGB").crop(box) for p in sys.argv[1:3])
sys.exit(0 if ImageChops.difference(a, b).getbbox() else 1)
PY
  }
  # settle: wait until two shots 0.2 s apart are equal (animations done).
  settle() {
    local i
    for i in $(seq 25); do
      grim "$OUT/prev.png"; sleep 0.2; grim "$OUT/now.png"
      cmp -s "$OUT/prev.png" "$OUT/now.png" && return 0
    done
    fail "the screen did not settle"
  }
  # step NAME X Y: click, wait for the sheet to change, keep the shot as $OUT/NAME.png.
  step() {
    "$POINTER" 1280 720 move "$2" "$3" > /dev/null
    "$POINTER" 1280 720 move "$2" "$3" click 272 move 5 715 > /dev/null
    wait_for 5 panel_changed || fail "$1: the sheet did not change"
    settle
    grim "$OUT/$1.png"; cp "$OUT/$1.png" "$OUT/last.png"
  }

  "$NOCTALIA" msg theme-mode-set light > /dev/null
  wait_for 5 theme_is light || fail "theme did not become light"
  grim "$OUT/last.png"
  "$NOCTALIA" msg panel-open control-center quick > /dev/null
  wait_for 5 panel_changed || fail "the sheet did not open"
  settle
  grim "$OUT/1-page1.png"; cp "$OUT/1-page1.png" "$OUT/last.png"

  "$POINTER" 1280 720 move 1150 152 click 272 move 5 715 > /dev/null
  wait_for 5 theme_is dark || fail "Dark Mode tile did not switch the theme to dark"
  wait_for 5 panel_changed || fail "the sheet did not repaint dark"
  settle
  grim "$OUT/10-dark.png"; cp "$OUT/10-dark.png" "$OUT/last.png"


  step 3-wifi-detail 1070 87
  step 4-back 940 80
  step 5-page2 1091 196
  step 6-page1 1079 196

  step 7-edit 951 284
  step 8-removed 932 108
  step 9-done 1224 76

  conf=$HOME/.local/state/noctalia/overrides.toml
  wait_for 5 bash -c "[[ -n \"$conf\" ]] && grep -q quick_tiles \"$conf\"" || fail "quick_tiles was not saved"
  tiles=$(sed -n "/quick_tiles/,/]/p" "$conf")
  [[ $tiles == *bluetooth* ]] || fail "saved tiles lost Bluetooth: $tiles"
  [[ $tiles != *wifi* ]] || fail "saved tiles still hold Wi-Fi: $tiles"
'
