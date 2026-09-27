#!/usr/bin/env bash
# `fps-overlay-toggle` draws each output's refresh readout in its top-right corner and a second toggle removes it: the
# corner changes after the first toggle and matches the original after the second. Writes before.png, overlay.png,
# after.png, and result.txt to $OUT (default ./artifacts/fps-overlay); overlay.png is the readout to look at.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/fps-overlay}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  sleep 2 # real time: the bar and wallpaper settle
  grim "$OUT/before.png"
  "$UMBRIEL" msg fps-overlay-toggle > /dev/null
  sleep 2.5 # real time: two readout refreshes, so the second shows a full second of presents
  grim "$OUT/overlay.png"
  "$UMBRIEL" msg fps-overlay-toggle > /dev/null
  sleep 0.5
  grim "$OUT/after.png"
'
python3 - "$OUT" <<'PY' | tee "$OUT/result.txt"
import sys
from PIL import Image, ImageChops
out = sys.argv[1]
def corner(name):
    image = Image.open(f"{out}/{name}.png").convert("RGB")
    w, h = image.size
    return image.crop((w - 420, 0, w, 90))
shown = ImageChops.difference(corner("before"), corner("overlay")).getbbox() is not None
removed = ImageChops.difference(corner("before"), corner("after")).getbbox() is None
print(f"overlay_shown={shown} overlay_removed={removed}")
sys.exit(0 if shown and removed else 1)
PY
echo "PASS; artifacts: $OUT"
