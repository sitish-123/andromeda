#!/usr/bin/env bash
# Compositor settings from Settings (gap 3.4). Fails when:
#   - the compositor rejects a change because config.toml has no include line (settings.toml loads without one),
#   - a value config.toml sets beats the value Settings saved (Settings wins),
#   - clearing a key does not fall back to what config.toml says,
#   - a non-input key (appearance, layout, animation) cannot be changed,
#   - Settings has no Window Style, Layout, Overview, or Behavior page on Umbriel, or they paint identically,
#   - `settings-open window-style` on a closed window lands elsewhere because the page did not exist yet.
# Writes settings.toml, the helper's transcript, and one screenshot per page to $OUT
# (default ./artifacts/compositor-settings).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/compositor-settings}
source "$(dirname "$0")/lib.sh"
boot_headless 1
printf '\n[appearance]\nborder_width = 6\n' >> "$RUNTIME/umbriel.toml"
run "$UMBRIEL" msg config-reload > /dev/null

SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/dsk-proto.o" "$BUILD/desktop-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/settings-set" \
  "$(dirname "$0")/settings_set.cpp" "$SRC/src/wayland/settings_control.cpp" "$RUNTIME/dsk-proto.o" -lwayland-client

step() { run "$RUNTIME/settings-set" "$@" | tee -a "$OUT/settings-set.txt"; }
: > "$OUT/settings-set.txt"
step appearance.border_width 0
grep -q '^border_width = 0' "$RUNTIME/settings.toml" || { echo "FAIL: settings.toml lacks border_width = 0" >&2; exit 1; }
step layout.gap 20
step animation.windows_in.style fade
step layout.mode master
cp "$RUNTIME/settings.toml" "$OUT/settings.toml"

with_noctalia '
  shot() { "$NOCTALIA" msg settings-open "$1" > /dev/null; sleep 1.5; grim -g "0,40 1280x680" "$OUT/$1.png"; }  # real time: repaint; below the bar
  shot window-style  # first, while Settings opens and the compositor settings are still arriving
  for page in appearance motion layout overview windows; do shot "$page"; done
'

step appearance.border_width "" 6
grep -q '^border_width' "$RUNTIME/settings.toml" && { echo "FAIL: clearing left border_width in settings.toml" >&2; exit 1; }
! grep -q "^FAIL" "$OUT/settings-set.txt" || { echo "FAIL: see $OUT/settings-set.txt" >&2; exit 1; }

pages=(window-style appearance motion layout overview windows)
for page in "${pages[@]}"; do
  [[ -s $OUT/$page.png ]] || { echo "FAIL: no screenshot for $page" >&2; exit 1; }
done
repeated=$(for page in "${pages[@]}"; do md5sum < "$OUT/$page.png"; done | sort | uniq -d | head -1)
[[ -z $repeated ]] || { echo "FAIL: two compositor pages painted identically" >&2; exit 1; }
echo "PASS: compositor settings persist without an include, win over config.toml, clear back to it; artifacts: $OUT"
