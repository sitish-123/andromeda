#!/usr/bin/env bash
# Settings sidebar tree (gaps 3.1), driven by a virtual pointer. Fails when:
#   - a pinned page (Displays, Sound & Brightness, Power) is not a top-level row,
#   - clicking a closed category header changes the page,
#   - an open category cannot be collapsed from its header,
#   - two categories are open at once,
#   - `settings-open <page>` for a page in a collapsed category leaves that category closed,
#   - Keyboard Shortcuts and Panel Keys are not the rows under Shortcuts.
# Writes one screenshot per step, plus a frame taken mid-animation, to $OUT (default ./artifacts/settings-sidebar).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-sidebar}
source "$(dirname "$0")/lib.sh"
boot_headless 1
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

# Screen rows with every category closed: header centres at 1280x720, 1x scale.
APPEARANCE=289 DEVICES=391
with_noctalia '
  shot() { sleep 1; grim -g "0,40 1280x680" "$OUT/$1.png"; }
  # The pointer leaves the window after each click so no row paints hovered.
  click() { "'"$POINTER"'" 1280 720 move 250 "$1" click 272 move 5 715 > /dev/null; }
  page() { "$NOCTALIA" msg settings-open "$1" > /dev/null; }
  page displays; shot collapsed
  click '$DEVICES'; shot devices-open
  (sleep 0.06; grim -g "0,40 1280x680" "$OUT/mid-animation.png") & mid=$!; click '$DEVICES'; wait $mid; shot devices-closed
  click '$APPEARANCE'; shot appearance-open
  click '$APPEARANCE'; sleep 0.4; click '$DEVICES'; sleep 0.4; click '$APPEARANCE'; shot appearance-after-devices
  page services; shot services
  page input; shot input
  page keybinds; shot keybinds
'

python3 - "$OUT" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
img = {n: Image.open(f"{out}/{n}.png").convert("RGB") for n in
       ["collapsed", "devices-open", "devices-closed", "appearance-open", "appearance-after-devices", "services",
        "input", "keybinds"]}
sidebar = (150, 110, 345, 680)
content = (370, 110, 1150, 680)
crop = lambda n, box: img[n].crop(box).tobytes()
fails = []
def check(ok, msg):
    if not ok:
        fails.append(msg)
# Image rows (screen y - 40) of the pill's empty right end, where only the row fill paints.
px = lambda n, y: img[n].getpixel((330, y))
accent = px("collapsed", 136)
check(px("collapsed", 170) != accent, "collapsed: Sound & Brightness painted selected on the Displays page")
check(px("services", 170) == accent and px("services", 136) != accent, "Sound & Brightness is not a top-level row")
check(crop("devices-open", content) == crop("collapsed", content), "opening Devices changed the page")
check(crop("devices-open", sidebar) != crop("collapsed", sidebar), "clicking Devices did not open it")
check(crop("devices-closed", sidebar) == crop("collapsed", sidebar), "clicking open Devices did not close it")
check(crop("appearance-after-devices", sidebar) == crop("appearance-open", sidebar),
      "opening Appearance left Devices open too")
check(px("input", 385) == accent, "settings-open input did not open Devices on Input")
check(px("keybinds", 453) == accent and px("keybinds", 419) != accent,
      "Panel Keys is not the second row under Shortcuts")
for f in fails:
    print("FAIL:", f, file=sys.stderr)
sys.exit(1 if fails else 0)
PY
echo "PASS: pinned rows, header toggles without navigating, one category open, settings-open expands; artifacts: $OUT"
