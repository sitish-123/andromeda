#!/usr/bin/env bash
# Activity actions reach the systemd user manager: mock_systemd1.py on a private session bus records each call while
# activity_actions (built against the shell's own ActivityActions) disables, re-enables and stops units, checks the
# "still starts on demand" result for a socket-activated unit, and writes and undoes XDG autostart overrides in a
# scratch dir. System units (polkit) are covered by face_polkit.sh. Writes artifacts to $OUT
# (default ./artifacts/activity-actions). Needs no compositor.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/activity-actions}
rm -rf "$OUT"; mkdir -p "$OUT/scratch"
: > "$OUT/calls.txt"
SRC=$ROOT/shell
BIN=$OUT/activity-actions
g++ -std=c++23 -I"$SRC/src" -I"$ROOT/libdesk-ui/src" $(pkg-config --cflags gio-2.0) -o "$BIN" \
  "$(dirname "$0")/activity_actions.cpp" "$SRC/src/system/activity_actions.cpp" "$SRC/src/system/xdg_autostart.cpp" \
  "$SRC/src/dbus/systemd/systemd_units_service.cpp" "$SRC/src/dbus/systemd/systemd_unit_name.cpp" \
  "$SRC/src/dbus/system_interactive_call.cpp" "$SRC/src/dbus/session_bus.cpp" \
  "$ROOT/libdesk-ui/src/core/log.cpp" -lsdbus-c++ $(pkg-config --libs gio-2.0)

MOCK="$ROOT/tests/e2e/mock_systemd1.py"
dbus-run-session -- bash -c '
  set -euo pipefail
  python3 "$1" "$2/calls.txt" > "$2/mock.log" 2>&1 &
  mock=$!
  trap "kill $mock 2>/dev/null || true" EXIT
  for _ in $(seq 50); do grep -q ready "$2/mock.log" && break; sleep 0.1; done
  grep -q ready "$2/mock.log" || { echo "FAIL: mock systemd1 did not start: $(cat "$2/mock.log")"; exit 1; }
  "$3" "$2/scratch" | tee "$2/activity-actions.txt"
' _ "$MOCK" "$OUT" "$BIN"
grep -q "^PASS" "$OUT/activity-actions.txt" && ! grep -q "^FAIL" "$OUT/activity-actions.txt" \
  || { echo "activity-actions reported a failure" >&2; exit 1; }
for call in "DisableUnitFiles opencode.service False" "EnableUnitFiles opencode.service False False" "StopUnit opencode.service replace" "Reload"; do
  grep -qx "$call" "$OUT/calls.txt" || { echo "FAIL: mock never received: $call" >&2; exit 1; }
done
echo "PASS: stop, disable, undo and autostart override; artifacts: $OUT"
