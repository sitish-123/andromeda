#!/usr/bin/env bash
# A copied image shows in the clipboard preview, also after the launcher closes and opens again on the same image.
# Fails when the preview stays blank on the reopen. Writes first.png, reopen.png and noctalia.log to $OUT
# (default ./artifacts/launcher-clipboard-image).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/launcher-clipboard-image}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  # red_pixels PNG: pure red pixels, the copied picture in the preview.
  red_pixels() { python3 -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]).convert(\"RGB\"); print(sum(1 for p in im.getdata() if p[0] > 200 and p[1] < 40 and p[2] < 40))" "$1"; }
  frames() { grep -c "open to first frame" "$OUT/noctalia.log" || true; }
  # open_shows_red NAME: open the clipboard, wait for its first frame, then up to 2 s for the picture. 0 = shown.
  open_shows_red() {
    local before
    before=$(frames)
    "$NOCTALIA" msg panel-open clipboard > /dev/null
    for _ in $(seq 100); do (( $(frames) > before )) && break; sleep 0.05; done
    for _ in $(seq 20); do
      grim "$OUT/$1.png"
      (( $(red_pixels "$OUT/$1.png") > 2000 )) && return 0
      sleep 0.1
    done
    return 1
  }
  python3 -c "from PIL import Image; Image.new(\"RGB\", (240, 160), (255, 0, 0)).save(\"$OUT/red.png\")"
  wl-copy -t image/png < "$OUT/red.png"
  # The history takes the copy asynchronously, and a panel lists what the history held when it opened.
  shown=0
  for _ in $(seq 10); do
    open_shows_red first && { shown=1; break; }
    "$NOCTALIA" msg panel-close > /dev/null
  done
  (( shown )) || { echo "FAIL: the copied image never showed in the clipboard preview"; exit 1; }
  "$NOCTALIA" msg panel-close > /dev/null
  open_shows_red reopen || { echo "FAIL: the clipboard preview is blank after a reopen"; exit 1; }
'
echo "PASS; artifacts: $OUT"
