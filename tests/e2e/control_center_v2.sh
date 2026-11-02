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
    grim "$OUT/now.png" && python3 -I - "${1:-$OUT/last.png}" "$OUT/now.png" "${BOX:-880,60,1280,500}" <<PY
import sys
from PIL import Image, ImageChops
box = tuple(int(v) for v in sys.argv[3].split(","))
a, b = (Image.open(p).convert("RGB").crop(box) for p in sys.argv[1:3])
sys.exit(0 if ImageChops.difference(a, b).getbbox() else 1)
PY
  }
  # The pills and footer only: a keyboard focus ring on a tile must not decide a match.
  panel_matches() { ! BOX=880,205,1280,330 panel_changed "$1"; }
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
  grim "$OUT/last.png"; cp "$OUT/last.png" "$OUT/closed.png"
  "$NOCTALIA" msg panel-open control-center quick > /dev/null
  wait_for 5 panel_changed || fail "the sheet did not open"
  settle
  grim "$OUT/1-page1.png"; cp "$OUT/1-page1.png" "$OUT/last.png"

  step 3-wifi-detail 1070 87
  step 4-back 940 80
  # `/` opens the search; "wi" and Enter open the Wi-Fi page, the same screen the chevron gave.
  # A new virtual keyboard needs one key to get focus before the shell routes its keys: Pause goes first.
  key() { local a=(tap 119 tap 119) k; for k in "$@"; do a+=(tap "$k"); done; "$POINTER" 1280 720 "${a[@]}" > /dev/null; }
  # press_until REF KEY: a key can still be lost while the shell takes focus of a new virtual keyboard, so resend
  # until the screen matches REF (at most 3 sends).
  press_until() { local ref=$1 try; shift; for try in 1 2 3; do key "$@"; wait_for 2 panel_matches "$ref" && return 0; cp "$OUT/now.png" "$OUT/press-miss-$try.png"; done; return 1; }
  key 53
  wait_for 5 panel_changed || fail "/ did not open the search"
  cp "$OUT/now.png" "$OUT/last.png"
  key 17 23
  wait_for 5 panel_changed || fail "typing did not filter the search"
  key 28
  wait_for 5 panel_matches "$OUT/3-wifi-detail.png" || fail "the search did not find Wi-Fi"
  step 6d-back 940 80

  # A keyboard that already sent events needs no priming: "/" after Escape reopens the search. The pauses let the sheet
  # finish its height morph: the shell drops a key press that arrives while the card resizes (finding in .agent/m3b-detail.md).
  "$POINTER" 1280 720 tap 119 tap 119 tap 53 pause 700 tap 1 pause 700 tap 53 > /dev/null
  cp "$OUT/4-back.png" "$OUT/last.png"
  wait_for 5 panel_changed || fail "an unprimed / after Escape did not reopen the search"
  panel_matches "$OUT/closed.png" && fail "the sheet closed instead of reopening the search"
  # Escape: search -> sheet.
  press_until "$OUT/4-back.png" 1 || fail "Escape did not return from the search to the sheet"
  # Escape: detail page -> sheet -> closed.
  step 6e-wifi-again 1070 87
  press_until "$OUT/4-back.png" 1 || fail "Escape did not return from the Wi-Fi page to the sheet"
  press_until "$OUT/closed.png" 1 || fail "a second Escape did not close the sheet"
  "$NOCTALIA" msg panel-open control-center quick > /dev/null
  cp "$OUT/closed.png" "$OUT/last.png"
  wait_for 5 panel_changed || fail "the sheet did not reopen"
  settle
  grim "$OUT/6f-reopened.png"; cp "$OUT/6f-reopened.png" "$OUT/last.png"

  "$POINTER" 1280 720 move 1150 152 click 272 move 5 715 > /dev/null
  wait_for 5 theme_is dark || fail "Dark Mode tile did not switch the theme to dark"
  wait_for 5 panel_changed || fail "the sheet did not repaint dark"
  settle
  grim "$OUT/10-dark.png"; cp "$OUT/10-dark.png" "$OUT/last.png"


  step 5-page2 1091 196
  step 6-page1 1079 196

  # A drag that starts on the Dark Mode tile changes the page and does not toggle the theme.
  "$POINTER" 1280 720 move 1150 152 press 272 move 1100 152 move 1000 152 move 900 152 release 272 move 5 715 > /dev/null
  wait_for 5 panel_changed || fail "a drag on a tile did not change the page"
  settle
  theme_is dark || fail "the tile drag toggled the theme"
  cp "$OUT/now.png" "$OUT/6b-drag-page2.png"
  cp "$OUT/5-page2.png" "$OUT/last.png"
  panel_changed && fail "the tile drag did not land on page 2"
  cp "$OUT/6b-drag-page2.png" "$OUT/last.png"
  step 6c-page1 1079 196

  step 7-edit 951 284
  step 8-removed 932 108
  step 9-done 1224 76

  conf=$HOME/.local/state/noctalia/overrides.toml
  wait_for 5 bash -c "[[ -n \"$conf\" ]] && grep -q quick_tiles \"$conf\"" || fail "quick_tiles was not saved"
  tiles=$(sed -n "/quick_tiles/,/]/p" "$conf")
  [[ $tiles == *bluetooth* ]] || fail "saved tiles lost Bluetooth: $tiles"
  [[ $tiles != *wifi* ]] || fail "saved tiles still hold Wi-Fi: $tiles"
'
