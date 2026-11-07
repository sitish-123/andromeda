#!/usr/bin/env bash
# Wallpaper panel v2 (.agent/DESIGN.md "Wallpaper panel v2"): the panel opens on a folder of eight wallpapers in light
# and dark (shots for the design check), a search with no match shows the "No Results" state, Down moves from the
# search into the grid, and F stores the selected wallpaper as a favourite. Every step waits on state with a timeout.
# Shots go to $OUT (default ./artifacts/wallpaper-panel-v2).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/wallpaper-panel-v2}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT" "$RUNTIME/walls"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
python3 -I - "$RUNTIME/walls" <<'PY'
import sys
from PIL import Image
for i, (a, b) in enumerate([((20, 40, 90), (200, 120, 60)), ((10, 90, 80), (230, 220, 180)), ((90, 20, 60), (250, 170, 90)),
                            ((30, 30, 30), (120, 140, 200)), ((200, 210, 230), (60, 80, 140)), ((240, 200, 160), (90, 50, 40)),
                            ((60, 120, 60), (200, 230, 120)), ((15, 15, 40), (90, 60, 160))], 1):
    img = Image.new("RGB", (640, 400))
    img.putdata([tuple(int(a[c] + (b[c] - a[c]) * (x + y) / 1040) for c in range(3)) for y in range(400) for x in range(640)])
    img.save(f"{sys.argv[1]}/wall-{i}.png")
PY
boot_headless 1
printf '\n[wallpaper]\ndirectory = "%s"\n' "$RUNTIME/walls" >> "$RUNTIME/home/.config/noctalia/config.toml"

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }
  theme_is() { [[ $("$NOCTALIA" msg theme-mode-get 2> /dev/null) == *"$1"* ]]; }
  settle() {
    local i
    for i in $(seq 25); do
      grim "$OUT/prev.png"; sleep 0.2; grim "$OUT/now.png"
      cmp -s "$OUT/prev.png" "$OUT/now.png" && return 0
    done
    fail "the screen did not settle"
  }
  changed_from() { grim "$OUT/now.png"; ! cmp -s "$1" "$OUT/now.png"; }
  # A new virtual keyboard needs one key to get focus before the shell routes its keys: Pause goes first.
  key() { local a=(tap 119 tap 119) k; for k in "$@"; do a+=(tap "$k"); done; "$POINTER" 1280 720 "${a[@]}" > /dev/null; }

  "$NOCTALIA" msg theme-mode-set light > /dev/null
  wait_for 5 theme_is light || fail "theme did not become light"
  "$NOCTALIA" msg panel-open wallpaper > /dev/null
  settle
  cp "$OUT/now.png" "$OUT/1-light.png"

  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  wait_for 5 theme_is dark || fail "theme did not become dark"
  settle
  cp "$OUT/now.png" "$OUT/2-dark.png"

  # z z z: no file name matches, the state view replaces the grid.
  for try in 1 2 3; do key 44 44 44; wait_for 3 changed_from "$OUT/2-dark.png" && break; done
  settle
  cp "$OUT/now.png" "$OUT/3-no-matches.png"
  cmp -s "$OUT/2-dark.png" "$OUT/3-no-matches.png" && fail "the search did not change the panel"

  # Backspace x3 restores the grid, Down enters it, F stores the first wallpaper as a favourite.
  key 14 14 14
  settle
  # [wallpaper] favorite in overrides.toml lists the stored favourites.
  favourite_saved() {
    local f
    f=$(find "$RUNTIME/home" -name overrides.toml -print -quit)
    [[ -n $f ]] && python3 -I -c "import sys,tomllib; d=tomllib.load(open(sys.argv[1],\"rb\")); sys.exit(0 if any(\"wall-\" in str(x) for x in d.get(\"wallpaper\",{}).get(\"favorite\",[])) else 1)" "$f"
  }
  favourite_saved && fail "a favourite existed before F"
  key 108 33
  wait_for 5 favourite_saved || fail "F did not store a favourite"
  settle
  cp "$OUT/now.png" "$OUT/4-favourite.png"
  echo PASS
'
