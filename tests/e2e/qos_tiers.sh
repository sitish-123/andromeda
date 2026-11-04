#!/usr/bin/env bash
# App QoS tiers (docs/qos.md §3-§4) on the real systemd user manager: a window in an app scope moves to the
# efficiency CPUs once hidden past the T2 delay and back on focus; with qos = "freeze_when_hidden" on battery its
# scope freezes at T3 and thaws while the app switcher surface exists. Delays and the battery are test-only
# overrides. Needs a hybrid CPU with efficiency cores. Writes result.txt to $OUT (default ./artifacts/qos-tiers).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/qos-tiers}
source "$(dirname "$0")/lib.sh"
CLIENT=${CLIENT:-$ROOT/compositor/build-debug/tests/background-client}
LAYER=${LAYER:-$ROOT/compositor/build-debug/tests/layer-client}
USER_BUS=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus
[[ -S $USER_BUS ]] || { echo "no systemd user bus at $USER_BUS" >&2; exit 1; }
UNIT=app-qos-e2e-$$.scope

# The compositor's private runtime dir reaches the real user manager through this link.
ln -s "$USER_BUS" "$RUNTIME/bus"
mkdir -p "$RUNTIME/power/BAT0"
echo Battery > "$RUNTIME/power/BAT0/type"
echo Charging > "$RUNTIME/power/BAT0/status"
export ANDROMEDA_QOS_TEST_DELAYS_MS=1000,1500 ANDROMEDA_QOS_TEST_POWER_SUPPLY=$RUNTIME/power
trap 'systemctl --user thaw "$UNIT" 2>/dev/null; systemctl --user stop "$UNIT" 2>/dev/null; systemctl --user stop "app-andromeda-${ADOPTED:-0}.scope" 2>/dev/null; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
# Spawned by the compositor without a scope (no scopes required here): it must get its own app scope.
AUTOSTART="\"$CLIENT qos-adopt\""
boot_headless 1
COMPOSITOR=$!
cat >> "$RUNTIME/andromeda.toml" <<'EOF'

[[window_rule]]
match.title = "^qos-freeze$"
qos = "freeze_when_hidden"
EOF
run "$ANDROMEDA" msg config-reload > /dev/null

fail() {
  echo "FAIL: $*" >&2
  tail -n 20 "$OUT/andromeda.log" >&2
  exit 1
}
ms() { echo $(($(date +%s%N) / 1000000)); }
# expand "14-15,3" -> "3,14,15"
expand() {
  local part cpus=()
  IFS=, read -ra parts <<< "$1"
  for part in "${parts[@]}"; do
    if [[ $part == *-* ]]; then
      for ((cpu = ${part%-*}; cpu <= ${part#*-}; cpu++)); do cpus+=("$cpu"); done
    else
      cpus+=("$part")
    fi
  done
  printf '%s\n' "${cpus[@]}" | sort -n | paste -sd,
}
cpus() { expand "$(awk '/^Cpus_allowed_list/ {print $2}' "/proc/$PID/status")"; }
# cgroup.events "frozen" is the state the kernel reached; cgroup.freeze is only the last request.
freeze() { awk '$1 == "frozen" {print $2}' "$CGROUP/cgroup.events"; }
weight() { cat "$CGROUP/cpu.weight"; }
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

EFFICIENCY=$(sed -n 's/.*efficiency CPUs for hidden apps: //p' "$OUT/andromeda.log" | head -n 1)
[[ -n $EFFICIENCY && $EFFICIENCY != none ]] || fail "no efficiency CPUs detected (log: '$EFFICIENCY')"
: > "$OUT/result.txt"

for _ in $(seq 100); do ADOPTED=$(pgrep -P "$COMPOSITOR" -fx "$CLIENT qos-adopt" || true); [[ -n $ADOPTED ]] && break; sleep 0.05; done
[[ -n $ADOPTED ]] || fail "autostart client never started"
scope_of() { sed -n 's|^0::.*/||p' "/proc/$ADOPTED/cgroup"; }
await "unscoped app: own scope" "app-andromeda-$ADOPTED.scope" 3000 scope_of

systemd-run --user --scope --quiet --unit="$UNIT" \
  env WAYLAND_DISPLAY="$RUNTIME/wayland-0" XDG_RUNTIME_DIR="$RUNTIME" "$CLIENT" qos-freeze > "$OUT/client.log" 2>&1 &
for _ in $(seq 100); do grep -q '^mapped$' "$OUT/client.log" 2>/dev/null && break; sleep 0.05; done
grep -q '^mapped$' "$OUT/client.log" || fail "client never mapped: $(cat "$OUT/client.log")"
CGROUP=/sys/fs/cgroup$(systemctl --user show -P ControlGroup "$UNIT")
PID=$(head -n 1 "$CGROUP/cgroup.procs")
ALL=$(cpus)
echo "unit $UNIT pid $PID cpus $ALL efficiency $EFFICIENCY" | tee -a "$OUT/result.txt"
await "focused: T0 weight 400" 400 2000 weight

# On AC: T2 (efficiency CPUs, weight 30) after the hidden delay, never T3.
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
start=$(ms)
await "hidden: T2 on efficiency CPUs" "$EFFICIENCY" 4000 cpus
(($(ms) - start >= 900)) || fail "T2 came before the hidden delay"
[[ $(weight) == 30 ]] || fail "T2 weight $(weight)"
run "$ANDROMEDA" msg workspace-switch:1 > /dev/null
await "focused again: every CPU" "$ALL" 1000 cpus
await "focused again: weight 400" 400 1000 weight

# On battery with freeze_when_hidden: T3 is one efficiency CPU and a frozen scope.
echo Discharging > "$RUNTIME/power/BAT0/status"
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
await "hidden long on battery: frozen" 1 6000 freeze
await "hidden long on battery: one efficiency CPU" "${EFFICIENCY%%,*}" 1000 cpus

# The switcher surface thaws it at once; closing the switcher with the app still hidden freezes it again.
env WAYLAND_DISPLAY="$RUNTIME/wayland-0" XDG_RUNTIME_DIR="$RUNTIME" LAYER_NAMESPACE=noctalia-window-switcher \
  "$LAYER" HEADLESS-1 32 overlay-layer > "$OUT/switcher.log" 2>&1 &
SWITCHER=$!
await "switcher open: thawed" 0 1000 freeze
kill "$SWITCHER"
wait "$SWITCHER" 2>/dev/null || true
await "switcher closed: frozen again" 1 2000 freeze
# Q6: shown from frozen, the app draws a new frame within 50 ms of the switch request (thaw + frame callback).
frames() { grep -c '^frame$' "$OUT/client.log" || true; }
resumes() { grep -c '^resumed$' "$OUT/client.log" || true; }
before=$(frames)
resumed_before=$(resumes)
start=$(ms)
run "$ANDROMEDA" msg workspace-switch:1 > /dev/null
sent=$(($(ms) - start))
resumed=
while (($(frames) <= before)); do
  [[ -z $resumed ]] && (($(resumes) > resumed_before)) && resumed=$(($(ms) - start))
  (($(ms) - start > 1000)) && fail "shown: no new frame within 1000 ms (request $sent ms, resumed ${resumed:-never})"
  sleep 0.005
done
took=$(($(ms) - start))
echo "shown: new frame in $took ms (request $sent ms, resumed ${resumed:-with the frame} ms)" | tee -a "$OUT/result.txt"
((took <= 50)) || fail "shown: new frame took $took ms, budget 50 ms"
await "shown: thawed" 0 1000 freeze
# A freeze still in flight at the thaw must not win afterwards.
for _ in {1..15}; do [[ $(freeze) == 0 ]] || fail "shown: frozen again"; sleep 0.02; done
await "shown: every CPU" "$ALL" 1000 cpus
echo PASS | tee -a "$OUT/result.txt"
