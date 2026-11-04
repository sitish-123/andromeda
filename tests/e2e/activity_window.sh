#!/usr/bin/env bash
# Activity window (S5). A foot window runs in a real transient scope that the private bus's mock systemd mirrors.
# Fails when:
#   - `msg activity-open act-test` maps no Activity window,
#   - the act-test row (tcp_chatter.py, about 2.5 MB/s over localhost) reports no TCP rate (net_bps) in the dump,
#   - the selected act-test row does not reach the dump,
#   - Delete while typing in the search field (Ctrl+F) quits the selected app,
#   - Delete (Quit on the selected row) does not stop the scope and close the foot window.
# Writes window.png (for the mockup crop at UI_SCALE, default 1) to $OUT (default ./artifacts/activity-window[-<scale>]).
set -euo pipefail
UI_SCALE=${UI_SCALE:-1}
OUT=${OUT:-$(pwd)/artifacts/activity-window-$UI_SCALE}
source "$(dirname "$0")/lib.sh"
REAL_RUNTIME=/run/user/$(id -u)
SCOPE="app-act\\x2dtest-$$.scope"
trap 'XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user stop "$SCOPE" > /dev/null 2>&1 || true; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
boot_headless 1
printf '\n[accessibility]\nui_scale = %s\n' "$UI_SCALE" >> "$RUNTIME/home/.config/noctalia/config.toml"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
export SCOPE REAL_RUNTIME POINTER=$ROOT/compositor/build-debug/tests/pointer-client
export CHATTER="$(cd "$(dirname "$0")" && pwd)/tcp_chatter.py"
BEFORE_NOCTALIA='REAL_RUNTIME=$REAL_RUNTIME python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done'
with_noctalia '
  XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --scope --unit="$SCOPE" \
    env XDG_RUNTIME_DIR="$RUNTIME" foot --config=/dev/null --title=act-test python3 "$CHATTER" > /dev/null 2>&1 &
  foot_pid=$!
  scope_cgroup=""
  for _ in $(seq 50); do
    scope_cgroup=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$SCOPE" 2> /dev/null || true)
    [[ -n $scope_cgroup ]] && break; sleep 0.1
  done
  [[ -n $scope_cgroup ]] || { echo "FAIL: foot scope did not start"; exit 1; }
  dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 \
    org.andromeda.MockSystemd.StartWithCgroup string:"$SCOPE" string:app.slice string:"$scope_cgroup" > /dev/null

  # The shell binds a new virtual keyboard after it appears and drops its first keys: hold one bound keyboard first.
  "$POINTER" 1280 720 keyboard-only mod none pause 300000 > /dev/null 2>&1 &
  for _ in $(seq 50); do grep -q "keyboard: bound" "$OUT/noctalia.log" && break; sleep 0.1; done
  grep -q "keyboard: bound" "$OUT/noctalia.log" || { echo "FAIL: the shell did not bind the held keyboard"; exit 1; }
  "$NOCTALIA" msg activity-open act-test > /dev/null
  for _ in $(seq 60); do
    "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
    jq -e '"'"'any(.rows[]; .app_id == "act-test" and .has_window and .memory_bytes > 0)'"'"' "$OUT/dump.json" > /dev/null 2>&1 && break
    sleep 0.1
  done
  jq -e '"'"'any(.rows[]; .app_id == "act-test" and .has_window)'"'"' "$OUT/dump.json" > /dev/null \
    || { echo "FAIL: act-test row not in the dump"; exit 1; }
  for _ in $(seq 50); do
    "$ANDROMEDA" windows --json > "$OUT/windows.json"
    jq -e '"'"'any(.. | objects; .app_id? == "dev.andromeda.Activity")'"'"' "$OUT/windows.json" > /dev/null && break
    sleep 0.1
  done
  jq -e '"'"'any(.. | objects; .app_id? == "dev.andromeda.Activity")'"'"' "$OUT/windows.json" > /dev/null \
    || { echo "FAIL: no Activity window mapped"; exit 1; }
  for _ in $(seq 50); do
    "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
    jq -e '"'"'any(.rows[]; .app_id == "act-test" and (.net_bps // 0) > 100000)'"'"' "$OUT/dump.json" > /dev/null && break
    sleep 0.1
  done
  jq -e '"'"'any(.rows[]; .app_id == "act-test" and (.net_bps // 0) > 100000)'"'"' "$OUT/dump.json" > /dev/null \
    || { echo "FAIL: no TCP rate for act-test: $(jq -c '"'"'.rows[] | select(.app_id == "act-test") | .net_bps'"'"' "$OUT/dump.json")"; exit 1; }
  grim "$OUT/window.png"

  alive() { local s; s=$(ps -o stat= -p "$foot_pid" 2> /dev/null || true); [[ -n $s && $s != Z* ]]; } # an exited child stays a zombie
  # Delete while the search field has focus edits the query; it must not quit the selected row.
  "$POINTER" 1280 720 mod control tap 33 mod none tap 33 tap 24 tap 24 tap 111 > /dev/null # Ctrl+F, "foo", Delete
  for _ in $(seq 10); do # a wrong quit shows up as a StopUnit within 1 s
    ! grep -qF "StopUnit $SCOPE" "$OUT/calls.txt" || { echo "FAIL: Delete in the search field quit the selected app"; exit 1; }
    sleep 0.1
  done
  alive || { echo "FAIL: foot exited while typing in search"; exit 1; }
  "$POINTER" 1280 720 tap 1 > /dev/null # Escape: leave the search field

  "$POINTER" 1280 720 tap 111 > /dev/null # Delete: Quit the selected row
  for _ in $(seq 50); do alive || break; sleep 0.1; done
  ! alive || { echo "FAIL: Quit did not close the foot window"; exit 1; }
  grep -qF "StopUnit $SCOPE" "$OUT/calls.txt" || { echo "FAIL: no StopUnit for $SCOPE"; exit 1; }
'
echo "PASS: Activity window opened with act-test selected; Quit closed the window; artifacts: $OUT"
