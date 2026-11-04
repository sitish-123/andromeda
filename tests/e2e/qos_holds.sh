#!/usr/bin/env bash
# org.andromeda.Compositor1.Apps holds (docs/system-health.md §5) on the real systemd user manager: List shows an app
# scope; a freeze hold does not touch the focused app, freezes it once hidden, and ends on Release and when its owner
# leaves the bus; a hold on an unknown unit fails. Writes result.txt to $OUT (default ./artifacts/qos-holds).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/qos-holds}
source "$(dirname "$0")/lib.sh"
CLIENT=${CLIENT:-$ROOT/compositor/build-debug/tests/background-client}
USER_BUS=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus
[[ -S $USER_BUS ]] || { echo "no systemd user bus at $USER_BUS" >&2; exit 1; }
UNIT=app-qos-holds-$$.scope
APPS=(org.andromeda.Compositor1 /org/andromeda/Compositor1/Apps org.andromeda.Compositor1.Apps)

# The compositor's private runtime dir reaches the real user bus through this link.
ln -s "$USER_BUS" "$RUNTIME/bus"
trap 'systemctl --user thaw "$UNIT" 2>/dev/null; systemctl --user stop "$UNIT" 2>/dev/null; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
boot_headless 1

fail() {
  echo "FAIL: $*" >&2
  tail -n 20 "$OUT/andromeda.log" >&2
  exit 1
}
ms() { echo $(($(date +%s%N) / 1000000)); }
freeze() { awk '$1 == "frozen" {print $2}' "$CGROUP/cgroup.events"; }
# await LABEL WANT LIMIT_MS CMD...: no event reports a cgroup change to a script, so this reads CMD every 20 ms.
await() {
  local label=$1 want=$2 limit=$3 start got
  shift 3
  start=$(ms)
  while got=$("$@"); [[ $got != "$want" ]]; do
    (($(ms) - start > limit)) && fail "$label: '$got', want '$want'"
    sleep 0.02
  done
  echo "$label in $(($(ms) - start)) ms" | tee -a "$OUT/result.txt"
}
: > "$OUT/result.txt"

for _ in $(seq 100); do busctl --user status "${APPS[0]}" > /dev/null 2>&1 && break; sleep 0.05; done
busctl --user status "${APPS[0]}" > /dev/null 2>&1 || fail "the compositor did not take ${APPS[0]}"

systemd-run --user --scope --quiet --unit="$UNIT" \
  env WAYLAND_DISPLAY="$RUNTIME/wayland-0" XDG_RUNTIME_DIR="$RUNTIME" "$CLIENT" qos-holds > "$OUT/client.log" 2>&1 &
for _ in $(seq 100); do grep -q '^mapped$' "$OUT/client.log" 2>/dev/null && break; sleep 0.05; done
grep -q '^mapped$' "$OUT/client.log" || fail "client never mapped: $(cat "$OUT/client.log")"
CGROUP=/sys/fs/cgroup$(systemctl --user show -P ControlGroup "$UNIT")
listed() { busctl --user call "${APPS[@]}" List | grep -c "\"$UNIT\"" || true; }
await "List shows the app" 1 2000 listed

# A holder that keeps its bus connection: a hold ends when its owner leaves the bus.
coproc HOLDER {
  python3 -u -c '
import sys, dbus
apps = dbus.Interface(dbus.SessionBus().get_object(sys.argv[1], sys.argv[2]), sys.argv[3])
for line in sys.stdin:
    cmd, arg = line.split()
    try:
        print(apps.HoldFreeze(arg, "e2e") if cmd == "freeze" else (apps.Release(arg), "released")[1], flush=True)
    except dbus.DBusException as e:
        print("error " + e.get_dbus_name(), flush=True)
' "${APPS[@]}"
}
ask() {
  echo "$1 $2" >&"${HOLDER[1]}"
  read -r -t 5 reply <&"${HOLDER[0]}" || fail "holder: no reply to $1"
  echo "$reply"
}

id=$(ask freeze "$UNIT")
[[ $id == apps-* ]] || fail "HoldFreeze: $id"
sleep 0.5
[[ $(freeze) == 0 ]] || fail "a freeze hold froze the focused app"
echo "focused app ignores the freeze hold" | tee -a "$OUT/result.txt"
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
await "hidden: frozen by the hold" 1 2000 freeze
[[ $(ask release "$id") == released ]] || fail "Release failed"
await "Release: thawed" 0 1000 freeze

id=$(ask freeze "$UNIT")
await "hold again: frozen" 1 1000 freeze
kill "$HOLDER_PID" 2> /dev/null || true
await "owner left the bus: thawed" 0 2000 freeze

unknown=$(busctl --user call "${APPS[@]}" HoldFreeze ss app-none-$$.scope e2e 2>&1 || true)
[[ $unknown == *"not an app scope"* ]] || fail "hold on an unknown unit: $unknown"
echo "unknown unit: refused" | tee -a "$OUT/result.txt"
echo "PASS: $OUT/result.txt"
