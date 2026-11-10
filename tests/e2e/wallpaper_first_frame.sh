#!/usr/bin/env bash
# The wallpaper surface maps only with its image: a large wallpaper decodes off the main thread, and the first frame
# the compositor shows from the background layer already holds the picture, never the empty black frame that ended
# the login hand-off early. Writes first.png and noctalia.log to $OUT (default ./artifacts/wallpaper-first-frame).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/wallpaper-first-frame}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
magick -size 7680x4320 xc:'#d04020' -quality 95 "$RUNTIME/wall.jpg"
cat > "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'TOML'
[shell]
setup_wizard_enabled = false
TOML
mkdir -p "$RUNTIME/home/.local/state/andromeda/shell"
cat > "$RUNTIME/home/.local/state/andromeda/shell/overrides.toml" <<TOML
[wallpaper.default]
path = "$RUNTIME/wall.jpg"
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  mapped() { "$ANDROMEDA" layers --json 2>/dev/null | python3 -c "import json,sys; sys.exit(not any(l[\"namespace\"]==\"noctalia-wallpaper\" and l[\"mapped\"] for l in json.load(sys.stdin)))"; }
  for _ in $(seq 400); do mapped && break; sleep 0.01; done
  mapped || fail "the wallpaper layer never mapped"
  grim "$OUT/first.png"
  rgb=$(magick "$OUT/first.png" -gravity center -crop 1x1+0+0 -format "%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]" info:)
  read -r r g b <<< "$rgb"
  (( r > 150 && g < 120 && b < 90 )) || fail "the first wallpaper frame is not the image (center rgb $rgb)"
  echo "PASS"
'
