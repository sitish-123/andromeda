#!/usr/bin/env bash
# Input settings reach the compositor's generated input.toml: drives dsk_input_manager_v1 directly (input_set.cpp,
# built against the shell's own InputControl client), then opens Settings > Input in the running Noctalia fork and
# screenshots it. Fails without the feature: input.toml never changes and the compositor has no dsk_input_manager_v1
# to advertise. Writes artifacts to $OUT (default ./artifacts/input-settings).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/input-settings}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/dsk-proto.o" "$BUILD/desktop-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/input-set" \
  "$(dirname "$0")/input_set.cpp" "$SRC/src/wayland/input_control.cpp" "$RUNTIME/dsk-proto.o" -lwayland-client

before=""
[[ -f $RUNTIME/input.toml ]] && before=$(cat "$RUNTIME/input.toml")

run "$RUNTIME/input-set" input.keyboard.repeat_rate 45 | tee "$OUT/input-set.txt"
grep -q "^PASS" "$OUT/input-set.txt" && ! grep -q "^FAIL" "$OUT/input-set.txt" || { echo "input-set reported a failure" >&2; exit 1; }

after=$(cat "$RUNTIME/input.toml")
[[ "$before" != "$after" ]] || { echo "FAIL: input.toml did not change" >&2; exit 1; }
grep -q 'repeat_rate = 45' "$RUNTIME/input.toml" || { echo "FAIL: input.toml missing repeat_rate = 45" >&2; exit 1; }
cp "$RUNTIME/input.toml" "$OUT/input.toml"

with_noctalia '
  "$NOCTALIA" msg settings-open input > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/input.png"
'
[[ -s $OUT/input.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: input.toml updated by dsk_input_manager_v1; artifacts: $OUT"
