#!/usr/bin/env bash
# Settings › Privacy and Security against mock_gaze.py on the test's private system bus: the Sign-in card shows the
# stored faces, the Face Unlock page lists them, and Remove (after the confirm alert) deletes one through gazed.
# Screenshots in $OUT (default ./artifacts/settings-privacy).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-privacy}
source "$(dirname "$0")/lib.sh"
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
rm -rf "$OUT"
boot_headless 1
: > "$OUT/calls.txt"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
BEFORE_NOCTALIA='
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock gaze did not start"; exit 1; }
' with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_calls() {  # wait_calls PATTERN COUNT (5 s)
    for _ in $(seq 50); do [ "$(grep -c "$1" "$OUT/calls.txt" || true)" -ge "$2" ] && return 0; sleep 0.1; done
    fail "mock never saw $1 x$2"
  }
  click() { "$POINTER" 1280 720 move "$1" "$2" click 272 > /dev/null; sleep 0.5; }
  "$NOCTALIA" msg settings-open privacy > /dev/null
  wait_calls "^ListFaces" 1
  sleep 0.5
  grim "$OUT/privacy-main.png"
  # Positions on the 1280x720 headless output (screenshots in $OUT show them).
  click 989 251    # Face Unlock chevron (only the chevron opens the page; the window may open narrow)
  grim "$OUT/privacy-face.png"
  click 969 250    # Remove on "With glasses"
  grim "$OUT/privacy-remove-alert.png"
  click 709 366    # alert Remove
  wait_calls "^DeleteFace .* With glasses" 1
  wait_calls "^ListFaces" 2
  sleep 0.5
  grim "$OUT/privacy-face-removed.png"
  # The Lock Screen page links to the face and fingerprint pages.
  "$NOCTALIA" msg settings-close > /dev/null
  "$NOCTALIA" msg settings-open lock-screen > /dev/null
  sleep 1
  grim "$OUT/lock-screen-link.png"
  # The private bus has no fprintd: the Fingerprint page says to install it, not that there is no reader.
  "$NOCTALIA" msg settings-close > /dev/null
  "$NOCTALIA" msg settings-open privacy > /dev/null
  sleep 1
  click 989 291    # Fingerprint chevron, one row under Face Unlock
  grim "$OUT/privacy-finger.png"
'
echo PASS
