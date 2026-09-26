#!/usr/bin/env bash
# harness: outputs=2
# wlr-output-power-management powers one output off and back on through the same path as the dpms actions, and
# reports the resulting mode to the client. The other output keeps its power.
set -euo pipefail

readonly CLIENT=${UMBRIEL_OUTPUT_POWER_CLIENT:-./build-debug/tests/output-power-client}

log_mark() { wc -l < "$UMBRIEL_LOG"; }
wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern" && return 0
    sleep 0.1
  done
  echo "timed out waiting for log: $pattern"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  return 1
}

mark=$(log_mark)
off=$("$CLIENT" HEADLESS-2 off)
[[ $off == *"mode off"* ]] || { echo "client was not told the output is off: $off"; exit 1; }
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "output 'HEADLESS-1': powered off"; then
  echo "powering HEADLESS-2 off also powered HEADLESS-1 off"
  exit 1
fi

mark=$(log_mark)
on=$("$CLIENT" HEADLESS-2 on)
[[ $on == *"mode on"* ]] || { echo "client was not told the output is on: $on"; exit 1; }
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode"

echo "output power protocol turns one output off and on and reports each mode"
