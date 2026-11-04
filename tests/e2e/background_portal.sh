#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.Background. Fails when:
#   - GetAppState does not list a mapped foot window (app id bg.test) as running,
#   - closing it emits no RunningApplicationsChanged or leaves it in GetAppState,
#   - NotifyBackground does not answer response 0 with result 1 (allow), posts no notification with Show, or its
#     Show action does not map the Activity window.
# Writes state-open.txt, state-closed.txt, signals.txt and notify.txt to $OUT (default ./artifacts/background-portal).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/background-portal}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  impl() {
    busctl --user call org.freedesktop.impl.portal.desktop.noctalia /org/freedesktop/portal/desktop \
      org.freedesktop.impl.portal.Background "$@"
  }
  foot --config=/dev/null --app-id=bg.test sh -c "sleep 300" > /dev/null 2>&1 &
  foot_pid=$!
  for _ in $(seq 50); do impl GetAppState > "$OUT/state-open.txt" 2>&1 && grep -q "\"bg.test\"" "$OUT/state-open.txt" && break; sleep 0.1; done
  grep -q "\"bg.test\" u [12]" "$OUT/state-open.txt" || { echo "FAIL: bg.test not running in GetAppState"; exit 1; }

  dbus-monitor --session "type=signal,interface=org.freedesktop.impl.portal.Background,member=RunningApplicationsChanged" \
    > "$OUT/signals.txt" 2>&1 &
  sleep 0.5 # real time: the monitor subscribes
  kill "$foot_pid"
  for _ in $(seq 50); do grep -q RunningApplicationsChanged "$OUT/signals.txt" && break; sleep 0.1; done
  grep -q RunningApplicationsChanged "$OUT/signals.txt" || { echo "FAIL: no RunningApplicationsChanged on close"; exit 1; }
  impl GetAppState > "$OUT/state-closed.txt"
  ! grep -q "\"bg.test\"" "$OUT/state-closed.txt" || { echo "FAIL: bg.test still in GetAppState"; exit 1; }

  impl NotifyBackground oss /org/freedesktop/portal/desktop/request/1_1/t bg.test "BG Test" > "$OUT/notify.txt"
  grep -q "^ua{sv} 0 1 \"result\" u 1$" "$OUT/notify.txt" || { echo "FAIL: NotifyBackground replied $(cat "$OUT/notify.txt")"; exit 1; }
  for _ in $(seq 50); do grep -q "portal background: bg.test" "$OUT/noctalia.log" && break; sleep 0.1; done
  grep -q "portal background: bg.test" "$OUT/noctalia.log" || { echo "FAIL: no Activity notification for bg.test"; exit 1; }
  [[ $("$NOCTALIA" msg notification-action-latest show) == ok ]] || { echo "FAIL: the notification has no Show action"; exit 1; }
  for _ in $(seq 50); do
    "$ANDROMEDA" windows --json > "$OUT/windows.json"
    jq -e "any(.. | objects; .app_id? == \"dev.andromeda.Activity\")" "$OUT/windows.json" > /dev/null && break
    sleep 0.1
  done
  jq -e "any(.. | objects; .app_id? == \"dev.andromeda.Activity\")" "$OUT/windows.json" > /dev/null \
    || { echo "FAIL: Show did not open Activity"; exit 1; }
'
echo "PASS; artifacts: $OUT"
