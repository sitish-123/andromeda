#!/usr/bin/env bash
# Date & Time settings reach org.freedesktop.timedate1: a mock timedate1 on the test's private system bus records
# each call while timedate_set (built against the shell's own TimeDateService client) sets a timezone, and the
# running Noctalia fork's Settings > Date & Time page is screenshotted before and after (its own TimeDateService
# instance sees the same PropertiesChanged signal). Writes artifacts to $OUT (default ./artifacts/date-time).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/date-time}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
g++ -std=c++23 -I"$SRC/src" -o "$RUNTIME/timedate-set" \
  "$(dirname "$0")/timedate_set.cpp" "$SRC/src/dbus/timedate/timedate_service.cpp" "$SRC/src/dbus/system_bus.cpp" \
  "$SRC/src/core/log.cpp" -lsdbus-c++

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_timedate1.py"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock timedate1 did not start: $(cat "$OUT/mock.log")"; exit 1; }

  "$NOCTALIA" msg settings-open date-time > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/date-time-before.png"

  DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" "$RUNTIME/timedate-set" | tee "$OUT/timedate-set.txt"
  grep -q "^PASS" "$OUT/timedate-set.txt" && ! grep -q "^FAIL" "$OUT/timedate-set.txt" \
    || { echo "timedate-set reported a failure"; exit 1; }
  grep -qx "SetTimezone Europe/Berlin True" "$OUT/calls.txt" \
    || { echo "FAIL: mock never received SetTimezone Europe/Berlin"; exit 1; }

  sleep 1 # real time: the shells own TimeDateService rebuilds the page from PropertiesChanged
  grim "$OUT/date-time-after.png"
  kill "$mock"
'
[[ -s $OUT/date-time-before.png && -s $OUT/date-time-after.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: timedate1.SetTimezone reached and reflected; artifacts: $OUT"
