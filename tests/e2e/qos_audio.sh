#!/usr/bin/env bash
# A hidden app that plays audio stays T1 (docs/qos.md §2-§3, Q4): the shell reports the PipeWire stream's pid with
# dsk_shell_v1 app_activity, so the app keeps every CPU past the hidden delay; once the stream stops, it drops to T2.
# Uses the real systemd user manager and the real PipeWire: it plays 6 s of silence. Needs efficiency cores.
# Writes result.txt to $OUT (default ./artifacts/qos-audio).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/qos-audio}
# Before lib.sh, which defaults NOCTALIA to shell/build-debug.
NOCTALIA=${NOCTALIA:-$(cd "$(dirname "$0")/../.." && pwd)/shell/build-f7/noctalia}
source "$(dirname "$0")/lib.sh"
CLIENT=${CLIENT:-$ROOT/compositor/build-debug/tests/background-client}
REAL=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
[[ -S $REAL/bus && -S $REAL/pipewire-0 ]] || { echo "needs the systemd user bus and PipeWire in $REAL" >&2; exit 1; }
UNIT=app-qos-audio-$$.scope

# The compositor reaches the real user manager and the shell the real PipeWire through these links.
ln -s "$REAL/bus" "$RUNTIME/bus"
ln -s "$REAL/pipewire-0" "$RUNTIME/pipewire-0"
# On AC (an empty power-supply dir): a hidden silent app stops at T2.
mkdir -p "$RUNTIME/power"
export ANDROMEDA_QOS_TEST_DELAYS_MS=1000,1500 ANDROMEDA_QOS_TEST_POWER_SUPPLY=$RUNTIME/power
trap 'systemctl --user stop "$UNIT" 2>/dev/null; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
boot_headless 1

fail() {
  echo "FAIL: $*" | tee -a "$OUT/result.txt" >&2
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

# The shell on a private session bus (no real BlueZ or NetworkManager), with the real PipeWire.
run dbus-run-session --config-file="$RUNTIME/bus.conf" -- \
  sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS exec "$0"' "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
for _ in $(seq 100); do run "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
run "$NOCTALIA" msg settings-close > /dev/null 2>&1 || fail "the shell did not start: $(tail -n 3 "$OUT/noctalia.log")"

systemd-run --user --scope --quiet --unit="$UNIT" \
  env WAYLAND_DISPLAY="$RUNTIME/wayland-0" XDG_RUNTIME_DIR="$RUNTIME" "$CLIENT" qos-audio > "$OUT/client.log" 2>&1 &
for _ in $(seq 100); do grep -q '^mapped$' "$OUT/client.log" 2>/dev/null && break; sleep 0.05; done
grep -q '^mapped$' "$OUT/client.log" || fail "client never mapped: $(cat "$OUT/client.log")"
CGROUP=/sys/fs/cgroup$(systemctl --user show -P ControlGroup "$UNIT")
PID=$(head -n 1 "$CGROUP/cgroup.procs")
ALL=$(cpus)

# Silence from a process in the app's scope: QoS matches activity by unit, as for a browser's audio process.
python3 -c 'import sys, wave; w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(bytes(48000 * 4 * 6))' "$RUNTIME/silence.wav"
pw-play --volume 0 "$RUNTIME/silence.wav" &
PLAYER=$!
echo "$PLAYER" > "$CGROUP/cgroup.procs"
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
# Past the 1 s hidden delay a silent app would be on the efficiency CPUs; this one plays, so it keeps every CPU.
start=$(ms)
while (($(ms) - start < 3000)); do
  if [[ $(cpus) != "$ALL" ]]; then
    for p in $(sed -n 's/.*app activity pid=\([0-9]*\).*/\1/p' "$OUT/andromeda.log" | sort -u); do
      echo "activity pid $p: $(tr '\0' ' ' < "/proc/$p/cmdline") cgroup $(cat "/proc/$p/cgroup")" >> "$OUT/result.txt"
    done
    fail "hidden while playing: CPUs $(cpus), want $ALL (player $PLAYER cgroup: $(cat /proc/$PLAYER/cgroup))"
  fi
  sleep 0.05
done
echo "hidden while playing: every CPU for 3000 ms" | tee -a "$OUT/result.txt"
kill "$PLAYER"
wait "$PLAYER" 2>/dev/null || true
await "stopped playing: T2 on efficiency CPUs" "$EFFICIENCY" 4000 cpus
echo PASS | tee -a "$OUT/result.txt"
