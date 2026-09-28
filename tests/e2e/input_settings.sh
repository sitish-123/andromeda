#!/usr/bin/env bash
# Input settings reach the compositor's generated settings.toml: drives dsk_settings_manager_v1 directly (input_set.cpp,
# built against the shell's own SettingsControl client), then opens Settings > Input in the running Noctalia fork and
# screenshots it. Fails without the feature: settings.toml never changes and the compositor has no dsk_settings_manager_v1
# to advertise. Writes artifacts to $OUT (default ./artifacts/input-settings).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/input-settings}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/dsk-proto.o" "$BUILD/desktop-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/settings-set" \
  "$(dirname "$0")/settings_set.cpp" "$SRC/src/wayland/settings_control.cpp" "$RUNTIME/dsk-proto.o" -lwayland-client

before=""
[[ -f $RUNTIME/settings.toml ]] && before=$(cat "$RUNTIME/settings.toml")

run "$RUNTIME/settings-set" input.keyboard.repeat_rate 45 | tee "$OUT/settings-set.txt"
grep -q "^PASS" "$OUT/settings-set.txt" && ! grep -q "^FAIL" "$OUT/settings-set.txt" || { echo "settings-set reported a failure" >&2; exit 1; }

after=$(cat "$RUNTIME/settings.toml")
[[ "$before" != "$after" ]] || { echo "FAIL: settings.toml did not change" >&2; exit 1; }
grep -q 'repeat_rate = 45' "$RUNTIME/settings.toml" || { echo "FAIL: settings.toml missing repeat_rate = 45" >&2; exit 1; }
cp "$RUNTIME/settings.toml" "$OUT/settings.toml"

with_noctalia '
  "$NOCTALIA" msg settings-open input > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/input.png"
'
[[ -s $OUT/input.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: settings.toml updated by dsk_settings_manager_v1; artifacts: $OUT"
