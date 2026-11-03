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
  sleep 1.5
  "$NOCTALIA" msg panel-open launcher > /dev/null; sleep 1.2
  grim "$OUT/'"$TAG"'-pill.png"
  "$NOCTALIA" msg panel-close launcher > /dev/null 2>&1 || true; sleep 0.6
  "$NOCTALIA" msg panel-open control-center > /dev/null; sleep 1.2
  grim "$OUT/'"$TAG"'-cc.png"
'
echo "captures: $OUT/$TAG-pill.png $OUT/$TAG-cc.png"
