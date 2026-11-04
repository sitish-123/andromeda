#!/usr/bin/env bash
# SystemdUnitsService reads a systemd user manager: mock_systemd1.py owns org.freedesktop.systemd1 on a private
# session bus with the opencode trigger case, an app unit, a D-Bus activated unit, a scope and units that must be
# skipped; activity_units (built against the shell's own client) checks the list, then a live start and stop.
# Writes artifacts to $OUT (default ./artifacts/activity-units). Needs no compositor.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/activity-units}
mkdir -p "$OUT"
: > "$OUT/calls.txt"
SRC=$ROOT/shell
BIN=$OUT/activity-units
g++ -std=c++23 -I"$SRC/src" -I"$ROOT/libdesk-ui/src" -o "$BIN" \
  "$(dirname "$0")/activity_units.cpp" "$SRC/src/dbus/systemd/systemd_units_service.cpp" \
  "$SRC/src/dbus/systemd/systemd_unit_name.cpp" "$SRC/src/dbus/session_bus.cpp" \
  "$ROOT/libdesk-ui/src/core/log.cpp" -lsdbus-c++

MOCK="$ROOT/tests/e2e/mock_systemd1.py"
dbus-run-session -- bash -c '
  set -euo pipefail
  python3 "$1" "$2/calls.txt" > "$2/mock.log" 2>&1 &
  mock=$!
  trap "kill $mock 2>/dev/null || true" EXIT
  for _ in $(seq 50); do grep -q ready "$2/mock.log" && break; sleep 0.1; done
  grep -q ready "$2/mock.log" || { echo "FAIL: mock systemd1 did not start: $(cat "$2/mock.log")"; exit 1; }
  "$3" | tee "$2/activity-units.txt"
' _ "$MOCK" "$OUT" "$BIN"
grep -q "^PASS" "$OUT/activity-units.txt" && ! grep -q "^FAIL" "$OUT/activity-units.txt" \
  || { echo "activity-units reported a failure" >&2; exit 1; }
grep -qx Subscribe "$OUT/calls.txt" && grep -qx Unsubscribe "$OUT/calls.txt" \
  || { echo "FAIL: live mode did not Subscribe and Unsubscribe" >&2; exit 1; }
echo "PASS: systemd user units listed and followed live; artifacts: $OUT"
