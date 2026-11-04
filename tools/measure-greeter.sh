#!/usr/bin/env bash
# Greeter budget (TASK-lockscreen G3): first frame and memory of a greeter client in a headless Andromeda, against
# mock_greetd.py (password-only PAM) and an empty private system bus. First frame = the client's own libdesk-ui
# "first frame at N ms after process start" log line (an output commit is no proof: mapping a window commits before
# the client draws). Memory is read once the output is idle for 300 ms.
# Env: GREETER (a command; the shell's greeter mode later), ANDROMEDA, RUNS (5), OUT. Results: $OUT/results.txt.
set -euo pipefail
OUT=${OUT:-/tmp/measure-greeter}
source "$(dirname "$0")/../tests/e2e/lib.sh"
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
GREETER=${GREETER:-$ROOT/greeter/build-release/noctalia-greeter --user $USER}
RUNS=${RUNS:-5}
rm -rf "$OUT"
boot_headless 1

dbus-daemon --config-file="$RUNTIME/bus.conf" --nofork --print-address=3 3> "$RUNTIME/system-bus" &
for _ in $(seq 100); do [[ -s $RUNTIME/system-bus ]] && break; sleep 0.05; done
SYSBUS=$(head -1 "$RUNTIME/system-bus")
mkdir -p "$RUNTIME/share/wayland-sessions"
cp "$ROOT/compositor/data/andromeda.desktop" "$RUNTIME/share/wayland-sessions/"

commits() { run "$ANDROMEDA" output-commits --json | python3 -Ic 'import json,sys;d=json.load(sys.stdin);print(d.get("ok",d).get("HEADLESS-1",0))'; }
now_ms() { echo $(($(date +%s%N) / 1000000)); }
field() { awk -v k="$1:" '$1 == k { print $2 }' "/proc/$2/status"; }
median() { sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }'; }

for i in $(seq 0 $((RUNS - 1))); do
  sock=$RUNTIME/greetd-$i.sock
  MOCK_SCENARIOS=noface python3 -I "$ROOT/tests/e2e/mock_greetd.py" "$sock" "$OUT/requests-$i.jsonl" > "$OUT/mock-$i.log" 2>&1 &
  mock=$!
  for _ in $(seq 100); do grep -q ready "$OUT/mock-$i.log" && break; sleep 0.05; done
  mkdir -p "$RUNTIME/greeter-state-$i"
  start=$(now_ms)
  # shellcheck disable=SC2086 # GREETER is a command line
  run env XDG_DATA_DIRS="$RUNTIME/share:/usr/share" NOCTALIA_GREETER_STATE_DIR="$RUNTIME/greeter-state-$i" \
    DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS" GREETD_SOCK="$sock" NOCTALIA_GREETER_LOG=stderr $GREETER > "$OUT/greeter-$i.log" 2>&1 &
  pid=$! spid=$!
  until grep -q 'first frame at' "$OUT/greeter-$i.log"; do
    kill -0 "$pid" 2> /dev/null || { echo "run $i: greeter exited, see $OUT/greeter-$i.log" >&2; exit 1; }
    (($(now_ms) - start < 20000)) || { echo "run $i: no first frame" >&2; exit 1; }
    sleep 0.01
  done
  ff=$(grep -o 'first frame at [0-9]*' "$OUT/greeter-$i.log" | head -1 | grep -o '[0-9]*$')
  last=$(commits) quiet=$(now_ms)
  while (($(now_ms) - quiet < 300)); do
    sleep 0.05
    c=$(commits); [[ $c != "$last" ]] && last=$c quiet=$(now_ms)
    (($(now_ms) - start < 30000)) || { echo "run $i: output never went idle" >&2; exit 1; }
  done
  # $! is the subshell that runs lib.sh's run(); the greeter is its deepest child.
  while c=$(pgrep -P "$pid" | head -1) && [[ -n $c ]]; do pid=$c; done
  cp "/proc/$pid/smaps" "$OUT/smaps-$i.txt"
  rss=$(field VmRSS "$pid") anon=$(field RssAnon "$pid") thr=$(field Threads "$pid")
  echo "$i $ff $((rss / 1024)) $((anon / 1024)) $thr $(cat "/proc/$pid/comm")" | tee -a "$OUT/runs.txt"
  kill "$pid" "$mock" 2> /dev/null || true
  wait "$spid" "$mock" 2> /dev/null || true
done

{
  echo "greeter: $GREETER"
  echo "runs: run first_frame_ms rss_mb anon_mb threads process (run 0 = first start in this boot)"
  cat "$OUT/runs.txt"
  tail -n +2 "$OUT/runs.txt" | awk '{ print $2 }' | median | sed 's/^/median first frame ms (runs 1+): /'
  tail -n +2 "$OUT/runs.txt" | awk '{ print $3 }' | median | sed 's/^/median RSS MB: /'
  tail -n +2 "$OUT/runs.txt" | awk '{ print $4 }' | median | sed 's/^/median RssAnon MB: /'
} > "$OUT/results.txt"
cat "$OUT/results.txt"
