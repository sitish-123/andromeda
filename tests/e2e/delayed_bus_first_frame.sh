#!/usr/bin/env bash
# M0 §5: a system bus whose services (mock UPower and logind) answer every call after DELAY seconds must not delay
# the shell's first frame. Start marker: just before Noctalia is launched. First frame: the first HEADLESS-1 compositor
# commit after that marker (an idle compositor commits nothing until the bar maps). Runs a control (no delay) and a delayed run; the
# delayed run must reach the first frame within MARGIN_MS of the control, and under BOUND_MS absolute (a blocking
# call would cost at least DELAY, so 1500 ms catches one 2 s stall). Writes times.txt and noctalia logs to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/delayed-bus-first-frame}
DELAY=${DELAY:-2}
MARGIN_MS=${MARGIN_MS:-1000}
BOUND_MS=${BOUND_MS:-1500}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
: > "$OUT/times.txt"
HERE="$(cd "$(dirname "$0")" && pwd)"

first_frame_ms() {
  local name=$1 delay=$2
  boot_headless 1
  run env NOCTALIA="$NOCTALIA" OUT="$OUT" ANDROMEDA="$ANDROMEDA" HERE="$HERE" NAME="$name" DELAY="$delay" \
    dbus-run-session --config-file="$RUNTIME/bus.conf" -- bash -c '
      set -euo pipefail
      export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS MOCK_DELAY=$DELAY
      trap '"'"'kill $(jobs -p) 2>/dev/null || true; wait'"'"' EXIT
      python3 "$HERE/mock_logind.py" "$OUT/$NAME.logind" > "$OUT/$NAME.logind.log" 2>&1 &
      python3 "$HERE/mock_power.py" "$OUT/$NAME.power" > "$OUT/$NAME.power.log" 2>&1 &
      for _ in $(seq 100); do grep -q ready "$OUT/$NAME.logind.log" "$OUT/$NAME.power.log" 2>/dev/null \
        && [[ $(grep -l ready "$OUT/$NAME.logind.log" "$OUT/$NAME.power.log" | wc -l) == 2 ]] && break; sleep 0.05; done
      commits() { "$ANDROMEDA" output-commits --json | jq -r ".\"HEADLESS-1\" // .ok.\"HEADLESS-1\" // 0"; }
      for _ in $(seq 100); do "$ANDROMEDA" output-commits --json > /dev/null 2>&1 && break; sleep 0.05; done
      base=$(commits)
      start=$(date +%s%N)
      "$NOCTALIA" > "$OUT/$NAME.noctalia.log" 2>&1 &
      for _ in $(seq 1000); do
        (( $(commits) > base )) && { echo $(( ($(date +%s%N) - start) / 1000000 )); exit 0; }
        sleep 0.01
      done
      echo "no first frame" >&2; exit 1
    ' > "$OUT/$name.ms"
  kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true
  rm -f "$RUNTIME"/wayland-0*
}

first_frame_ms control 0; control=$(cat "$OUT/control.ms")
echo "control first_frame_ms=$control" | tee -a "$OUT/times.txt"
first_frame_ms delayed "$DELAY"; delayed=$(cat "$OUT/delayed.ms")
echo "delayed first_frame_ms=$delayed (every reply +${DELAY}s)" | tee -a "$OUT/times.txt"

if (( delayed > control + MARGIN_MS || delayed > BOUND_MS )); then
  echo "FAIL: delayed first frame ${delayed} ms vs control ${control} ms (margin ${MARGIN_MS}, bound ${BOUND_MS})"
  exit 1
fi
echo "PASS: first frame ${delayed} ms with delayed bus, ${control} ms without"
