#!/usr/bin/env bash
# Removable drives mount themselves through UDisks2: with a mock org.freedesktop.UDisks2 on the test's private system
# bus, a hotplugged filesystem with HintAuto is mounted and announced, a system filesystem is left alone, and the
# notification's click opens the drive (the default action). Writes calls.txt to $OUT (default ./artifacts/drives).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/drives}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_udisks.py"
with_noctalia '
  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  shell=$!
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  plug() {
    busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call org.freedesktop.UDisks2 /org/freedesktop/UDisks2 \
      dsk.test.UDisks Plug sbb "$1" "$2" "$3" > /dev/null
  }
  plug SYSTEMFS true true
  plug USBSTICK true false
  for _ in $(seq 50); do grep -q "mounted USBSTICK" "$OUT/noctalia.log" && break; sleep 0.1; done
  grep -q "mounted USBSTICK at /run/media/test/USBSTICK" "$OUT/noctalia.log" \
    || { echo "FAIL: the removable drive was not mounted"; exit 1; }
  grep -q "Mount SYSTEMFS" "$OUT/calls.txt" && { echo "FAIL: a system filesystem was mounted"; exit 1; }
  kill "$mock" "$shell"
'
echo "PASS: $(tr '\n' ',' < "$OUT/calls.txt"); artifacts: $OUT"
