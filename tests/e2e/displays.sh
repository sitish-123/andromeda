#!/usr/bin/env bash
# Boots a headless Umbriel with two outputs. Drives OutputManagement through apply, disable, and revert against it
# (output-apply.txt), then runs the Noctalia fork, opens Settings > Displays, and screenshots it (displays.png), and
# again after a protocol mirror (displays-mirror.png). Artifacts go to $OUT (default ./artifacts/displays).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/displays}
source "$(dirname "$0")/lib.sh"
boot_headless 2

SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/proto.o" "$BUILD/wlr-output-management-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/output-apply" \
  "$(dirname "$0")/output_apply.cpp" "$SRC/src/wayland/output_management.cpp" "$RUNTIME/proto.o" -lwayland-client

run "$UMBRIEL" outputs > "$OUT/outputs-before.txt"
run "$RUNTIME/output-apply" | tee "$OUT/output-apply.txt"
run "$UMBRIEL" outputs > "$OUT/outputs-after.txt"
with_noctalia '
  "$NOCTALIA" msg settings-open displays > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/displays.png"
  "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
  sleep 1 # real time: the page rebuilds from the mirror event
  grim "$OUT/displays-mirror.png"
  "$DESKTOP_CLIENT" state > "$OUT/mirror-state.txt"
'
[[ -s $OUT/displays.png && -s $OUT/displays-mirror.png ]] || { echo "no screenshot" >&2; exit 1; }
grep -qx "HEADLESS-2 HEADLESS-1" "$OUT/mirror-state.txt" || { echo "mirror not reported" >&2; exit 1; }
echo "artifacts: $OUT"
