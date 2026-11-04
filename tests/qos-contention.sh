#!/usr/bin/env bash
# QoS Q3 (docs/qos.md §9): frame times of a focused app next to a busy hidden app, with QoS off and on.
# The focused app runs a 60 Hz frame loop whose work is split into one 4 ms chunk per performance CPU, run in parallel
# (as a browser raster or a game frame): a one-thread app does not feel the load, because cgroup group scheduling
# already gives its scope half the machine. The hidden app runs one busy loop per CPU.
# frame_ms = time from the frame's deadline to the end of its last chunk; missed = frames over 16.7 ms.
# Uses the real systemd user manager (temporary app scopes) and full CPU load for ROUNDS x 2 x ~15 s.
# Usage: ROUNDS=3 tests/qos-contention.sh. Prints one line per run; appends them to bench/qos-contention.tsv.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [[ ${1:-} != inner ]]; then
  ROUNDS=${ROUNDS:-3}
  for round in $(seq "$ROUNDS"); do
    for mode in off on; do
      QOS_MODE=$mode ROUND=$round bash "$0" inner
    done
  done
  exit 0
fi

OUT=${OUT:-$ROOT/artifacts/qos-contention}/$QOS_MODE-$ROUND
source "$ROOT/tests/e2e/lib.sh"
CLIENT=${CLIENT:-$ROOT/compositor/build-debug/tests/background-client}
REAL=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FOCUSED=app-qos-focused-$$.scope
HIDDEN=app-qos-hidden-$$.scope
ln -s "$REAL/bus" "$RUNTIME/bus"
mkdir -p "$RUNTIME/power"
export ANDROMEDA_QOS_TEST_DELAYS_MS=1000,1500 ANDROMEDA_QOS_TEST_POWER_SUPPLY=$RUNTIME/power
[[ $QOS_MODE == off ]] && export ANDROMEDA_QOS_DISABLE=1
trap 'systemctl --user stop "$FOCUSED" "$HIDDEN" 2>/dev/null; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
boot_headless 1

# scope UNIT TITLE: a mapped window in a new app scope; prints the scope's cgroup dir.
scope() {
  systemd-run --user --scope --quiet --unit="$1" \
    env WAYLAND_DISPLAY="$RUNTIME/wayland-0" XDG_RUNTIME_DIR="$RUNTIME" "$CLIENT" "$2" > "$OUT/$2.log" 2>&1 &
  for _ in $(seq 100); do grep -q '^mapped$' "$OUT/$2.log" 2>/dev/null && break; sleep 0.05; done
  grep -q '^mapped$' "$OUT/$2.log" || { echo "$2 never mapped" >&2; exit 1; }
  echo "/sys/fs/cgroup$(systemctl --user show -P ControlGroup "$1")"
}

HIDDEN_CG=$(scope "$HIDDEN" qos-hidden)
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
FOCUSED_CG=$(scope "$FOCUSED" qos-focused)
# With QoS on, wait for T2: the hidden scope gets the efficiency CPUs once past the 1 s hidden delay.
if [[ $QOS_MODE == on ]]; then
  for _ in $(seq 200); do [[ -n $(cat "$HIDDEN_CG/cpuset.cpus" 2>/dev/null) ]] && break; sleep 0.02; done
  [[ -n $(cat "$HIDDEN_CG/cpuset.cpus") ]] || { echo "hidden app never reached T2" >&2; exit 1; }
fi
# Loop count for 4 ms of work, measured before the load starts, so both modes run the same work per frame.
WORK=$(python3 -c '
import time
def work(n):
    x = 0
    for _ in range(n):
        x += 1
n = 200000
start = time.perf_counter(); work(n); spent = time.perf_counter() - start
print(int(n * 0.004 / spent))')

for _ in $(seq "$(nproc)"); do
  sha256sum /dev/zero > /dev/null &
  echo $! > "$HIDDEN_CG/cgroup.procs"
done
# Performance CPUs: every CPU whose max frequency is above the lowest one.
CHUNKS=$(lscpu -e=MAXMHZ | tail -n +2 | sort -g | awk 'NR == 1 { low = $1 } $1 > low { n++ } END { print n ? n : NR }')
python3 - "$FOCUSED_CG/cgroup.procs" "$WORK" "$CHUNKS" > "$OUT/frames.txt" <<'PY'
import multiprocessing, os, sys, time
with open(sys.argv[1], "w") as procs:
    procs.write(str(os.getpid()))
def work(n):
    x = 0
    for _ in range(n):
        x += 1
    return x
n, chunks = int(sys.argv[2]), int(sys.argv[3])
with multiprocessing.get_context("fork").Pool(chunks) as pool:
    pool.map(work, [n] * chunks)
    period = 1 / 60
    deadline = time.perf_counter() + period
    for _ in range(600):
        time.sleep(max(0.0, deadline - time.perf_counter()))
        pool.map(work, [n] * chunks, chunksize=1)
        print(f"{(time.perf_counter() - deadline) * 1000:.3f}")
        deadline += period
PY
python3 - "$OUT/frames.txt" "$QOS_MODE" "$ROUND" <<'PY' | tee -a "$ROOT/bench/qos-contention.tsv"
import statistics, sys
ms = sorted(float(line) for line in open(sys.argv[1]))
p = lambda q: ms[min(len(ms) - 1, int(q * len(ms)))]
missed = sum(1 for v in ms if v > 1000 / 60)
print(f"qos={sys.argv[2]}\tround={sys.argv[3]}\tframes={len(ms)}\tp50={p(0.5):.2f}\tp99={p(0.99):.2f}\tmax={ms[-1]:.2f}\tmissed={missed}")
PY
