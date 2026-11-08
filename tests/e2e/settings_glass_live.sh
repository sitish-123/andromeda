#!/usr/bin/env bash
# Settings › Glass acts on the open Settings window at once: the window glass follows the slider without a reopen.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-glass-live}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/dsk-proto.o" "$BUILD/desktop-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$ROOT/libdesk-ui/src" -I"$BUILD" -o "$RUNTIME/settings-set" \
  "$(dirname "$0")/settings_set.cpp" "$SRC/src/compositors/settings_control.cpp" "$RUNTIME/dsk-proto.o" -lwayland-client
run "$RUNTIME/settings-set" appearance.glass 0 > /dev/null
export SETTINGS_SET=$RUNTIME/settings-set

with_noctalia '
  mean() { magick "$1" -colorspace gray -format "%[fx:round(1000*mean)]" info:; }
  shot() { sleep 1.5; grim -g "0,40 1280x680" "$OUT/$1.png"; mean "$OUT/$1.png"; }  # real time: the glass eases in
  "$NOCTALIA" msg settings-open appearance > /dev/null
  clear=$(shot clear)
  "$SETTINGS_SET" appearance.glass 1 > /dev/null
  frosted=$(shot frosted)
  "$SETTINGS_SET" appearance.glass 0 > /dev/null
  again=$(shot clear-again)
  echo "mean brightness x1000: clear $clear, frosted $frosted, clear again $again"
  diff=$((clear > frosted ? clear - frosted : frosted - clear))
  back=$((clear > again ? clear - again : again - clear))
  ((diff >= 5)) || { echo "FAIL: the open Settings window did not follow Glass 0 -> 1 ($clear vs $frosted)" >&2; exit 1; }
  ((back <= 2)) || { echo "FAIL: Glass back to 0 did not restore the window ($clear vs $again)" >&2; exit 1; }
'
echo "PASS: the open Settings window follows the Glass slider; artifacts: $OUT"
