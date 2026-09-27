#!/usr/bin/env bash
# wp_commit_timing_manager_v1 is offered, and an update stamped 400 ms ahead is drawn neither before its time nor long
# after it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/commit-timing.log"

"$CLIENT" commit-timing > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the timed window"
for _ in $(seq 40); do
  grep -q '_ms ' "$LOG" && break
  sleep 0.05
done
if grep -q '^early_ms' "$LOG"; then
  echo "the timed update was drawn $(sed -n 's/^early_ms //p' "$LOG") ms before its timestamp"
  exit 1
fi
late=$(sed -n 's/^late_ms //p' "$LOG")
if [[ -z $late ]] || ((late > 100)); then
  echo "the timed update was drawn ${late:-never} ms after its timestamp"
  exit 1
fi
echo "timed update drawn $late ms after its timestamp"
