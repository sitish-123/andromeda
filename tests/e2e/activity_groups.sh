#!/usr/bin/env bash
# Activity groups by unit and knows which units have a window. Two real transient units run on the user's systemd
# manager: a foot window in app-act\x2dtest-<pid>.scope and a windowless `sleep` in background.slice. The test
# shell's private bus has mock_systemd1.py mirror both units with their real ControlGroup, so ActivityMonitor reads
# their real cgroup files and maps the window PID through /proc. `msg activity-dump` must show the foot scope as
# app "act-test" with a window and the sleep service as a windowless background row with memory. Both units stop
# at exit. Writes artifacts to $OUT (default ./artifacts/activity-groups).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-groups}
source "$(dirname "$0")/lib.sh"
REAL_RUNTIME=/run/user/$(id -u)
SCOPE="app-act\\x2dtest-$$.scope"
BG="act-test-bg-$$.service"
cleanup_units() {
  XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user stop "$SCOPE" "$BG" > /dev/null 2>&1 || true
}
trap 'cleanup_units; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
boot_headless 1

XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --unit="$BG" --slice=background.slice sleep 300
BG_CGROUP=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$BG")
[[ -n $BG_CGROUP ]] || { echo "FAIL: no cgroup for $BG" >&2; exit 1; }

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
export SCOPE BG BG_CGROUP REAL_RUNTIME
BEFORE_NOCTALIA='python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done'
with_noctalia '
  XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --scope --unit="$SCOPE" \
    env XDG_RUNTIME_DIR="$RUNTIME" foot --config=/dev/null --title=act-test sh -c "sleep 300" > /dev/null 2>&1 &
  scope_cgroup=""
  for _ in $(seq 50); do
    scope_cgroup=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$SCOPE" 2> /dev/null || true)
    [[ -n $scope_cgroup ]] && break; sleep 0.1
  done
  [[ -n $scope_cgroup ]] || { echo "FAIL: foot scope did not start"; exit 1; }

  call() { dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 "org.andromeda.MockSystemd.$@" > /dev/null; }
  "$NOCTALIA" msg activity-dump > /dev/null # takes the 10 s lease: live mode and sampling start
  call StartWithCgroup string:"$SCOPE" string:app.slice string:"$scope_cgroup"
  call StartWithCgroup string:"$BG" string:background.slice string:"$BG_CGROUP"

  # Wait for the window to map and two samples to give rates.
  for _ in $(seq 60); do
    "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
    jq -e --arg bg "$BG" '"'"'any(.rows[]; .app_id == "act-test" and .has_window) and any(.rows[]; .key == $bg and .memory_bytes > 0)'"'"' "$OUT/dump.json" > /dev/null 2>&1 && break
    sleep 0.1
  done
'
jq . "$OUT/dump.json" > "$OUT/dump.pretty.json"
jq -e '.available and .sampling' "$OUT/dump.json" > /dev/null || { echo "FAIL: monitor not sampling" >&2; exit 1; }
jq -e --arg scope "$SCOPE" '.rows[] | select(.app_id == "act-test" and .has_window and any(.units[]; . == $scope))' "$OUT/dump.json" > /dev/null \
  || { echo "FAIL: foot scope is not the act-test app with a window" >&2; exit 1; }
jq -e ".rows[] | select(.key == \"$BG\" and (.has_window | not) and .slice == \"background\" and .memory_bytes > 0)" "$OUT/dump.json" > /dev/null \
  || { echo "FAIL: sleep service is not a windowless background row with memory" >&2; exit 1; }
echo "PASS: window unit and windowless background unit grouped; artifacts: $OUT"
