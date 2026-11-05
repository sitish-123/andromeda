#!/usr/bin/env bash
# Captures the launcher pill and control center at Clear glass over a full-screen terminal of text.
# Shows whether the rim lens shows sharp text and whether text under the glass stays readable.
# Usage: SCHEME=light|dark GLASS=0.0 tests/e2e/glass_clear_text.sh. Captures in $OUT (default ./artifacts/glass-clear-text).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/glass-clear-text}
ANDROMEDA=${ANDROMEDA:-$(cd "$(dirname "$0")/../.." && pwd)/compositor/build-live/andromeda}
SCHEME=${SCHEME:-light}
GLASS=${GLASS:-0.0}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
seq 1 400 | awk '{printf "%05d  The quick brown fox jumps over the lazy dog; sharp text under glass %s\n", $1, $1 * 7}' > "$RUNTIME/text.txt"

boot_headless 1
printf '\n[appearance]\nscheme = "%s"\nglass = %s\n' "$SCHEME" "$GLASS" >> "$RUNTIME/andromeda.toml"
# The shell's palette follows its own [theme] mode, not the compositor scheme.
printf '\n[theme]\nmode = "%s"\n' "$SCHEME" >> "$RUNTIME/home/.config/noctalia/config.toml"
TAG=$SCHEME-g$GLASS
with_noctalia '
  foot --fullscreen -o main.font=monospace:size=11 sh -c "cat '"$RUNTIME"'/text.txt; exec sleep 600" > "$OUT/foot.log" 2>&1 &
  # shoot NAME: wait until the screen differs from $OUT/base.png (the panel shows), then until two shots 0.2 s apart
  # are equal (its open animation is done); keep that shot as NAME.
  shoot() {
    for _ in $(seq 50); do
      grim "$OUT/now.png"; cmp -s "$OUT/base.png" "$OUT/now.png" || break; sleep 0.05
    done
    for _ in $(seq 25); do
      grim "$OUT/prev.png"; sleep 0.2; grim "$OUT/now.png"
      cmp -s "$OUT/prev.png" "$OUT/now.png" && { mv "$OUT/now.png" "$OUT/$1"; rm -f "$OUT/prev.png"; return 0; }
    done
    echo "the screen did not settle for $1" >&2; exit 1
  }
  sleep 1.5
  grim "$OUT/base.png"
  "$NOCTALIA" msg panel-open launcher > /dev/null
  shoot "'"$TAG"'-pill.png"
  "$NOCTALIA" msg panel-close launcher > /dev/null 2>&1 || true; sleep 0.6
  grim "$OUT/base.png"
  "$NOCTALIA" msg panel-open control-center > /dev/null
  shoot "'"$TAG"'-cc.png"
  rm -f "$OUT/base.png"
'
echo "captures: $OUT/$TAG-pill.png $OUT/$TAG-cc.png"
