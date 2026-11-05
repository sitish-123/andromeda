#!/usr/bin/env bash
# Shell wakeup and redraw budget: the headless shell's main thread per scenario, measured two ways:
# voluntary context switches of that thread, and the shell's own main-loop profiler (NOCTALIA_IDLE_PROFILE: loops,
# redraws, top wake sources). Scenarios: idle; media playing with no local audio stream (phone media over Link; a test
# MPRIS player whose Position does not advance), where the island waveform must still move (compositor dsk loop).
# Run after any change that can add timers, polling or redraws: `just e2e-one shell_wakeups`.
# Writes result.txt and per-scenario profiles to $OUT (default ./artifacts/shell-wakeups).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/shell-wakeups}
MAX_WAKEUPS=${MAX_WAKEUPS:-2} # per second, main thread, each scenario
export NOCTALIA_IDLE_PROFILE=1
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  # The test shell, not a live one that runs the same binary: match it by its runtime dir.
  pid=
  for p in $(pgrep -x noctalia); do
    tr "\0" "\n" < /proc/$p/environ 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$RUNTIME" && { pid=$p; break; }
  done
  [[ -n $pid ]] || { echo "FAIL: no test shell process"; exit 1; }
  switches() { awk "/^voluntary_ctxt_switches/ {print \$2}" /proc/$pid/task/$pid/status; }
  now() { date +%T.%3N; }
  fail=0
  # scenario NAME: 21 s, so at least one whole 10 s profiler window falls inside; reports the switch rate and the
  # profiler windows that start inside the scenario.
  scenario() {
    local name=$1 start before rate
    start=$(now)
    before=$(switches)
    sleep 21 # real time: the measurement window
    rate=$(awk -v n=$(( $(switches) - before )) "BEGIN {printf \"%.1f\", n / 21}")
    awk -v s="$start" -v e="$(now)" "{ t = \$1 } t >= s && t <= e" <(sed "s/\x1b\[[0-9;]*m//g" "$OUT/noctalia.log") \
      | grep "\[main\] idle profile" > "$OUT/profile-$name.txt" || true
    local loops redraws
    # The second profiler window lies wholly inside the scenario.
    loops=$(grep -E "idle profile [0-9.]+s:" "$OUT/profile-$name.txt" | sed -n "2s/.*idle profile \([0-9.]*\)s: loops=\([0-9]*\).*/\2 \1/p" | awk "{printf \"%.1f\", \$1 / \$2}")
    redraws=$(grep "idle profile surfaces:" "$OUT/profile-$name.txt" | sed -n "2s/.* renders=\([0-9]*\).*/\1/p")
    echo "$name: main thread wakeups/s $rate, loops/s ${loops:-?}, redraws/10s ${redraws:-?}" | tee -a "$OUT/result.txt"
    awk "/idle profile [0-9.]+s:/ {n++; next} n == 2 && /wake=/ && k++ < 5" "$OUT/profile-$name.txt" | sed "s/.*idle profile source //" | cut -c1-90 | sed "s/^/  /" | tee -a "$OUT/result.txt"
    awk -v r="$rate" "BEGIN {exit !(r <= '"$MAX_WAKEUPS"')}" || { echo "FAIL: $name: $rate wakeups/s (budget '"$MAX_WAKEUPS"')" | tee -a "$OUT/result.txt"; fail=1; }
  }

  : > "$OUT/result.txt"
  sleep 15 # real time: startup work ends (a startup HTTP request with no route times out after 10 s)
  scenario idle

  python3 "$(dirname "$NOCTALIA")/../../tests/e2e/mpris_test_player.py" "$OUT/player-calls.txt" &
  for _ in $(seq 50); do gdbus call --session -d org.freedesktop.DBus -o / -m org.freedesktop.DBus.NameHasOwner org.mpris.MediaPlayer2.e2e 2>/dev/null | grep -q true && break; sleep 0.1; done
  sleep 3 # real time: the island grows into the media face and the position probe gives up
  grim -g "0,0 1280x40" "$OUT/media-a.png"
  sleep 0.1 # real time: the waveform loop moves on the compositor clock
  grim -g "0,0 1280x40" "$OUT/media-b.png"
  cmp -s "$OUT/media-a.png" "$OUT/media-b.png" && { echo "FAIL: the island waveform did not move in 100 ms" | tee -a "$OUT/result.txt"; fail=1; }
  scenario media
  exit $fail
'
echo "PASS; artifacts: $OUT"
