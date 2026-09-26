#!/usr/bin/env bash
# Gap 1.13 (browser half): net/url_open.cpp opens URLs through GIO's g_app_info_launch_default_for_uri instead of
# spawning xdg-open. Builds a tiny standalone driver against the real url_open.cpp/log.cpp and calls it with no
# default handler configured (empty mimeapps.list, empty applications dir), watching every process on the machine
# for 3s around the call. xdg-open must never appear. Writes spawns.log to $OUT (default ./artifacts/native-spawns).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/native-spawns}
source "$(dirname "$0")/lib.sh"

SRC=$ROOT/shell/src
g++ -std=c++23 -I"$SRC" $(pkg-config --cflags gio-2.0) \
  -o "$RUNTIME/url-open-test" \
  "$(dirname "$0")/url_open_main.cpp" "$SRC/net/url_open.cpp" "$SRC/core/log.cpp" \
  $(pkg-config --libs gio-2.0)

# A runtime dir with no wayland-0/X11 socket: any GUI app GIO resolves as the URL handler fails to open a display
# immediately, so nothing ever draws a window on the real desktop, regardless of which handler is installed.
mkdir -p "$OUT" "$RUNTIME/home/.config" "$RUNTIME/home/.local/share/applications"
: > "$RUNTIME/home/.config/mimeapps.list"

: > "$OUT/spawns.log"
timeout 3 bash -c 'while true; do ps -eo comm=; sleep 0.02; done >> "$1"' _ "$OUT/spawns.log" &

env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS HOME="$RUNTIME/home" \
  XDG_DATA_HOME="$RUNTIME/home/.local/share" XDG_RUNTIME_DIR="$RUNTIME" \
  timeout 5 "$RUNTIME/url-open-test" "https://example.invalid/no-handler-configured" | tee "$OUT/result.txt"

sleep 0.5
pkill -9 -f "no-handler-configured" 2>/dev/null || true

grep -qE "^launched=" "$OUT/result.txt" || { echo "FAIL: url-open-test produced no result"; exit 1; }
if grep -qx "xdg-open" "$OUT/spawns.log"; then
  echo "FAIL: xdg-open spawned"
  exit 1
fi
echo "PASS: net::openInBrowser used GIO directly, no xdg-open spawn; artifacts: $OUT"
