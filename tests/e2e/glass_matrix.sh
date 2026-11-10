#!/usr/bin/env bash
# Glass legibility matrix: every glass surface over light, dark and colourful wallpapers, at Clear, Tinted and Frosted.
# Usage: SCHEME=light|dark tests/e2e/glass_matrix.sh. Shots: $OUT/<scheme>/<wall>-g<glass>-<surface>.png, and one board
# per surface: $OUT/board-<scheme>-<surface>.png (rows = wallpapers, columns = glass steps).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/artifacts/glass-matrix}
SCHEME=${SCHEME:-light}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT/$SCHEME"

# Three backdrops: a bright paper photo stand-in, the dark default galaxy, and a saturated colour field.
WALLS=$RUNTIME/walls
mkdir -p "$WALLS"
magick -size 1280x720 gradient:'#f4f1ea-#d9d2c6' -fill '#ffffff' -draw 'circle 900,200 900,420' -blur 0x30 "$WALLS/light.png"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$WALLS/dark.png"
magick -size 1280x720 -seed 7 plasma:'#ff3b6b-#2b7bff' -blur 0x8 -modulate 100,160 "$WALLS/colour.png"
seq 1 200 | awk '{printf "%04d  The quick brown fox jumps over the lazy dog %s\n", $1, $1 * 7}' > "$RUNTIME/text.txt"

boot_headless 1
printf '\n[appearance]\nscheme = "%s"\nglass = 0.5\n' "$SCHEME" >> "$RUNTIME/andromeda.toml"
printf '\n[theme]\nmode = "%s"\n' "$SCHEME" >> "$RUNTIME/home/.config/andromeda/shell/config.toml"
export RUNTIME WALLS SCHEME SCHEME_OUT=$OUT/$SCHEME
with_noctalia '
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; sleep 0.8; }
  close_all() {
    for p in launcher clipboard control-center; do "$NOCTALIA" msg panel-close "$p" > /dev/null 2>&1 || true; done
    settle
  }
  foot -o main.font=monospace:size=11 sh -c "cat $RUNTIME/text.txt; exec sleep 900" > /dev/null 2>&1 &
  sleep 2
  for wall in light dark colour; do
    "$NOCTALIA" msg wallpaper-set "$WALLS/$wall.png" > /dev/null
    for glass in 0.0 0.5 1.0; do
      sed -i "s/^glass = .*/glass = $glass/" "$RUNTIME/andromeda.toml"
      "$ANDROMEDA" msg config-reload > /dev/null
      settle
      tag=$SCHEME_OUT/$wall-g$glass
      grim "$tag-base.png"
      "$NOCTALIA" msg panel-open launcher > /dev/null; settle; grim "$tag-launcher.png"
      "$NOCTALIA" msg panel-open launcher apps > /dev/null; settle; grim "$tag-launcher-apps.png"; close_all
      "$NOCTALIA" msg panel-open clipboard > /dev/null; settle; grim "$tag-clipboard.png"; close_all
      "$NOCTALIA" msg panel-open control-center > /dev/null; settle; grim "$tag-cc.png"; close_all
      "$NOCTALIA" msg panel-open control-center session > /dev/null; settle; grim "$tag-power.png"; close_all
      "$NOCTALIA" msg panel-open control-center calendar > /dev/null; settle; grim "$tag-calendar.png"; close_all
      "$NOCTALIA" msg notification-show "Glass check $wall $glass" "Body text over the backdrop to read" > /dev/null
      settle; grim "$tag-toast.png"
    done
  done
'
# Boards: rows light/dark/colour wallpaper, columns Clear/Tinted/Frosted, each shot halved.
for surface in base launcher launcher-apps clipboard cc power calendar toast; do
  args=()
  for wall in light dark colour; do
    for glass in 0.0 0.5 1.0; do
      args+=("$OUT/$SCHEME/$wall-g$glass-$surface.png")
    done
  done
  magick montage "${args[@]}" -tile 3x3 -geometry 640x360+4+4 -background '#808080' "$OUT/board-$SCHEME-$surface.png"
done
ls "$OUT"/board-"$SCHEME"-*.png
