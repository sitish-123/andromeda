#!/usr/bin/env bash
# HEADLESS-2 mirrors HEADLESS-1, like a laptop panel mirroring a TV. The mirror has no wl_output, so the only way to
# power it is through its source: DPMS on the source, by compositor action or by wlr-output-power-management on the
# source's wl_output, must power the mirror off and back on with it. Writes power.txt to $OUT (default
# ./artifacts/mirror-power) plus the compositor log.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-power}
source "$(dirname "$0")/lib.sh"
boot_headless 2
POWER=$ROOT/compositor/build-debug/tests/output-power-client
: > "$OUT/power.txt"

mark() { wc -l < "$OUT/umbriel.log"; }
expect_log() {
  local from=$1 pattern=$2 label=$3
  for _ in $(seq 50); do
    tail -n +"$((from + 1))" "$OUT/umbriel.log" | grep -q "$pattern" && { echo "PASS $label" >> "$OUT/power.txt"; return; }
    sleep 0.1
  done
  echo "FAIL $label (no \"$pattern\" in the log)" >> "$OUT/power.txt"
}
commits() { run "$UMBRIEL" output-commits --json | jq '."HEADLESS-2"'; }

run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
expect_log 0 "output 'HEADLESS-2': mirroring 'HEADLESS-1'" "HEADLESS-2 mirrors HEADLESS-1"

from=$(mark)
run "$UMBRIEL" msg dpms-off > /dev/null
expect_log "$from" "output 'HEADLESS-1': powered off" "dpms-off powers the source off"
expect_log "$from" "output 'HEADLESS-2': powered off" "dpms-off powers the mirror off with its source"
from=$(mark)
run "$UMBRIEL" msg dpms-on > /dev/null
expect_log "$from" "output 'HEADLESS-2': applied mode" "dpms-on powers the mirror back on"
before=$(commits)
run foot --config=/dev/null sh -c 'sleep 30' > /dev/null 2>&1 &
for _ in $(seq 50); do (( $(commits) > before )) && break; sleep 0.1; done
(( $(commits) > before )) && echo "PASS the mirror draws again after power-on" >> "$OUT/power.txt" \
  || echo "FAIL the mirror did not draw after power-on" >> "$OUT/power.txt"

from=$(mark)
run "$POWER" HEADLESS-1 off > /dev/null
expect_log "$from" "output 'HEADLESS-2': powered off" "output-power off on the source's wl_output powers the mirror off"
from=$(mark)
run "$POWER" HEADLESS-1 on > /dev/null
expect_log "$from" "output 'HEADLESS-2': applied mode" "output-power on powers the mirror back on"

cat "$OUT/power.txt"
! grep -q '^FAIL' "$OUT/power.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
