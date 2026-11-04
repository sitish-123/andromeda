#!/usr/bin/env bash
# Captures every glass surface at one glass slider state, to compare Clear, Tinted, Frosted and Reduce Transparency.
# Usage: GLASS=0.0|0.5|1.0|rt SCHEME=light|dark tests/e2e/glass_slider_shots.sh. Captures in $OUT/<scheme>-g<GLASS>-*.png.
# Menus and tooltips have no IPC: a virtual pointer (pointer-client) hovers and right-clicks the dock.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/artifacts/glass-slider}
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/noctalia}
POINTER=${POINTER:-$ROOT/compositor/build-live/tests/pointer-client}
SCHEME=${SCHEME:-light}
GLASS=${GLASS:-0.5}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
seq 1 200 | awk '{printf "%04d  The quick brown fox jumps over the lazy dog %s\n", $1, $1 * 7}' > "$RUNTIME/text.txt"

boot_headless 1
if [[ $GLASS == rt ]]; then
  printf '\n[appearance]\nscheme = "%s"\nreduce_transparency = true\n' "$SCHEME" >> "$RUNTIME/andromeda.toml"
else
  printf '\n[appearance]\nscheme = "%s"\nglass = %s\n' "$SCHEME" "$GLASS" >> "$RUNTIME/andromeda.toml"
fi
# A translucent window with compositor blur and a forced server-side title bar: window glass (decoration + ssd).
printf '\n[[window_rule]]\nmatch.app_id = "^glassterm$"\nblur = true\ntitle_bar = "forced"\n' >> "$RUNTIME/andromeda.toml"
printf '\n[theme]\nmode = "%s"\n' "$SCHEME" >> "$RUNTIME/home/.config/noctalia/config.toml"
export TAG=$SCHEME-g$GLASS
with_noctalia '
  shot() { grim "$OUT/$TAG-$1.png"; }
  pointer() { "'"$POINTER"'" 1280 720 "$@" > /dev/null; }
  foot -o main.font=monospace:size=11 sh -c "cat '"$RUNTIME"'/text.txt; exec sleep 600" > "$OUT/foot.log" 2>&1 &
  foot --app-id glassterm -o colors-dark.alpha=0.5 -o colors-light.alpha=0.5 -o main.font=monospace:size=14 sh -c "echo window glass; exec sleep 600" \
    > "$OUT/foot2.log" 2>&1 &
  sleep 2
  shot base
  "$NOCTALIA" msg panel-open launcher > /dev/null; sleep 1.2; shot launcher
  "$NOCTALIA" msg panel-close launcher > /dev/null 2>&1 || true; sleep 0.6
  "$NOCTALIA" msg panel-open control-center > /dev/null; sleep 1.2; shot cc-attached
  "$NOCTALIA" msg panel-close control-center > /dev/null 2>&1 || true; sleep 0.6
  "$NOCTALIA" msg notification-show "Glass check" "Notification toast over the backdrop" > /dev/null; sleep 1.2; shot toast
  "$NOCTALIA" msg osd-show audio-volume-high 60 > /dev/null 2>&1 || true; sleep 0.8; shot osd
  sleep 2
  "$NOCTALIA" msg window-switcher > /dev/null 2>&1 || true; sleep 1.0; shot switcher
  "$NOCTALIA" msg window-switcher close > /dev/null 2>&1 || true; sleep 0.6
  # Dock icons at 1280x720: settings (610, 678), foot (667, 680).
  pointer move 610 678; sleep 1.5; shot tooltip
  pointer move 667 680 click 273; sleep 1.0; shot menu
  pointer move 5 300 click 272; sleep 0.6
  "$ANDROMEDA" msg overview-toggle > /dev/null; sleep 1.2; shot overview
  "$ANDROMEDA" msg overview-toggle > /dev/null; sleep 0.8
  "$ANDROMEDA" msg window-toggle-scratchpad > /dev/null 2>&1 || true; sleep 0.4
  "$ANDROMEDA" msg scratchpad-toggle > /dev/null 2>&1 || true; sleep 1.2; shot scratchpad
  "$ANDROMEDA" msg scratchpad-toggle > /dev/null 2>&1 || true; sleep 0.6
  "$NOCTALIA" msg session lock > /dev/null 2>&1 || true; sleep 1.5; shot lock
'
ls "$OUT"/"$TAG"-*.png
