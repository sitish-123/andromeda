#!/usr/bin/env bash
# Viewer P4 (docs/apps.md §8): open a 3-page PDF and search a word that is on pages 2 and 3.
# Drives the app through org.freedesktop.Application.ActivateAction ("search" takes the query). Fails when:
#   - no Viewer window maps, or page 1 is not rendered ("shown pdf page 1"),
#   - the search does not end with "1 of 2" in the log (the first hit current, two in all),
#   - the view does not scroll to the first hit on page 2.
# Writes viewer.log and viewer-pdf.png (the highlighted hit) to $OUT (default ./artifacts/viewer-pdf).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/viewer-pdf}
source "$(dirname "$0")/lib.sh"
VIEWER=${VIEWER:-$ROOT/apps/viewer/build/andromeda-viewer}
PREVIEW_HELPER=${PREVIEW_HELPER:-$ROOT/apps/viewer/build/subprojects/andromeda-preview/andromeda-preview-helper}
APP=dev.andromeda.Viewer
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$RUNTIME/files"
python3 -I - "$RUNTIME/files/sample.pdf" <<'PY'
import cairo, sys
s = cairo.PDFSurface(sys.argv[1], 595, 842)
c = cairo.Context(s)
for text in ["Page one has nothing", "Page two has a zephyr", "Page three has a zephyr too"]:
    c.set_font_size(28); c.move_to(72, 144); c.show_text(text); c.show_page()
s.finish()
PY

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" ANDROMEDA_PREVIEW_HELPER="$PREVIEW_HELPER" NOCTALIA_LOG_LEVEL=debug \
  "$VIEWER" "$RUNTIME/files/sample.pdf" > "$OUT/viewer.log" 2>&1 &

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { grep -q "$1" "$OUT/viewer.log"; }
mapped() { run "$ANDROMEDA" windows --json | jq -e "[.. | objects | select(.app_id? == \"$APP\")] | length > 0" > /dev/null; }
action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Viewer \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}

wait_for "no Viewer window" mapped
wait_for "page 1 was not rendered" logged "shown pdf page 1"
action search "[<'zephyr'>]"
wait_for "search did not find 2 hits" logged 'search "zephyr" 1 of 2'
wait_for "the view did not move to the first hit (page 2)" logged "shown hit page 2"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/viewer-pdf.png"
echo PASS
