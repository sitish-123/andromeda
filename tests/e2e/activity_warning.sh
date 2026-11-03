#!/usr/bin/env bash
# The background warning fires once for the trigger case and its Stop action reaches systemd. mock_systemd1.py
# serves opencode.service (enabled from ~/.config, started 3 h ago) whose ControlGroup points into a fake cgroup
# tree (NOCTALIA_CGROUP_ROOT) holding 480 MB; pipewire and the other mock units have no cgroup files and stay quiet.
# `msg activity-check` must post exactly one finding, a second check none (state survives in
# ~/.local/state/.../activity.json), and the notification's "stop" action must call StopUnit. Writes artifacts to $OUT
# (default ./artifacts/activity-warning), with settings.png: the Settings search for the Background Activity row.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-warning}
source "$(dirname "$0")/lib.sh"
boot_headless 1

CG=$RUNTIME/cgroup
OC=$CG/user.slice/user-1000.slice/user@1000.service/app.slice/opencode.service
mkdir -p "$OC"
printf 'usage_usec 10000000\nuser_usec 1\nsystem_usec 1\n' > "$OC/cpu.stat"
echo $((480 * 1024 * 1024)) > "$OC/memory.current"
printf 'some avg10=4.00 avg60=0.00 avg300=0.00 total=1\n' > "$OC/memory.pressure"
mkdir -p "$RUNTIME/home/.config/systemd/user"
: > "$RUNTIME/home/.config/systemd/user/opencode.service"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
export NOCTALIA_CGROUP_ROOT=$CG
BEFORE_NOCTALIA='python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done'
: > "$OUT/calls.txt"
with_noctalia '
  findings() { grep -c "background finding:" "$OUT/noctalia.log" || true; }
  "$NOCTALIA" msg activity-check > /dev/null
  for _ in $(seq 50); do [[ $(findings) -ge 1 ]] && break; sleep 0.1; done
  "$NOCTALIA" msg activity-check > /dev/null
  sleep 1 # real time: a second check would post within this second
  "$NOCTALIA" msg settings-open "search:background activity" > /dev/null
  sleep 1.5 # real time: the Settings window maps and paints
  grim "$OUT/settings.png"
  echo "findings=$(findings)" > "$OUT/result.txt"
  [[ -s $HOME/.local/state/noctalia/activity.json || -n $(find "$HOME/.local/state" -name activity.json 2>/dev/null) ]] \
    && echo "state=saved" >> "$OUT/result.txt"
  echo "action=$("$NOCTALIA" msg notification-action-latest stop)" >> "$OUT/result.txt"
  for _ in $(seq 50); do grep -q "^StopUnit opencode.service replace$" "$OUT/calls.txt" && break; sleep 0.1; done
'
cat "$OUT/result.txt"
grep "background finding:" "$OUT/noctalia.log" || true
grep -qx "findings=1" "$OUT/result.txt" || { echo "FAIL: want exactly one finding over two checks" >&2; exit 1; }
grep -q "background finding: opencode.service (uses resources" "$OUT/noctalia.log" || { echo "FAIL: opencode not flagged" >&2; exit 1; }
grep -qx "state=saved" "$OUT/result.txt" || { echo "FAIL: notified state not saved" >&2; exit 1; }
grep -qx "action=ok" "$OUT/result.txt" || { echo "FAIL: no stop action on the warning" >&2; exit 1; }
grep -qx "StopUnit opencode.service replace" "$OUT/calls.txt" || { echo "FAIL: Stop did not reach systemd" >&2; exit 1; }
echo "PASS: one warning for opencode, none repeated, Stop reached systemd; artifacts: $OUT"
