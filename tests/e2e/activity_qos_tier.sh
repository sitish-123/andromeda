#!/usr/bin/env bash
# P8 (docs/apps.md §8): Settings › Activity shows the compositor's QoS tier of each app. A foot window runs in a real
# transient app scope; the compositor, the mock systemd and the shell share one private bus at $RUNTIME/bus, so the
# shell reads org.andromeda.Compositor1.Apps of this headless compositor, not the session's one.
# Fails when:
#   - the tier-test row has no tier while its window shows,
#   - the row does not show `hidden` after its workspace is switched away (test hidden delay 1 s).
# Writes the dumps and logs to $OUT (default ./artifacts/activity-qos-tier).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-qos-tier}
source "$(dirname "$0")/lib.sh"
REAL_RUNTIME=/run/user/$(id -u)
SCOPE="app-tier\\x2dtest-$$.scope"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
fail() { echo "FAIL: $*"; exit 1; }

bus_info=$(dbus-daemon --session --fork --address="unix:path=$RUNTIME/bus" --print-pid=1)
BUS="unix:path=$RUNTIME/bus"
trap 'XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user stop "$SCOPE" > /dev/null 2>&1 || true; kill '"$bus_info"' $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
export ANDROMEDA_QOS_TEST_DELAYS_MS=1000,1500
boot_headless 1

onbus() { run env DBUS_SESSION_BUS_ADDRESS="$BUS" DBUS_SYSTEM_BUS_ADDRESS="$BUS" "$@"; }
onbus env REAL_RUNTIME="$REAL_RUNTIME" python3 "$MOCK" "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
for _ in $(seq 50); do grep -q ready "$OUT/mock.log" 2> /dev/null && break; sleep 0.1; done
grep -q ready "$OUT/mock.log" || fail "the mock systemd did not start"
onbus "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
for _ in $(seq 100); do onbus "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done

XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --scope --unit="$SCOPE" \
  env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 foot --config=/dev/null --title=tier-test sleep 600 \
  > /dev/null 2>&1 &
cgroup=""
for _ in $(seq 50); do
  cgroup=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$SCOPE" 2> /dev/null || true)
  [[ -n $cgroup ]] && break
  sleep 0.1
done
[[ -n $cgroup ]] || fail "the foot scope did not start"
onbus dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 \
  org.andromeda.MockSystemd.StartWithCgroup string:"$SCOPE" string:app.slice string:"$cgroup" > /dev/null

onbus "$NOCTALIA" msg activity-open tier-test > /dev/null
tier() {
  onbus "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
  jq -r '[.rows[] | select(.app_id == "tier-test") | .tier][0] // ""' "$OUT/dump.json"
}
shown=""
for _ in $(seq 60); do shown=$(tier); [[ $shown == focused || $shown == visible ]] && break; sleep 0.1; done
[[ $shown == focused || $shown == visible ]] || fail "tier-test shows tier '$shown' while its window shows"
echo "PASS shown window: tier $shown"

run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
for _ in $(seq 60); do shown=$(tier); [[ $shown == hidden ]] && break; sleep 0.1; done
[[ $shown == hidden ]] || fail "tier-test shows tier '$shown' after its workspace was switched away"
echo "PASS hidden window: tier hidden"
