#!/usr/bin/env bash
# The shell's screen-off and screen-on use wlr-output-power-management: `noctalia msg dpms-off` powers both headless
# outputs off and `dpms-on` powers them back on, and the shell's Wayland trace shows the set_mode requests. Writes
# power.txt to $OUT (default ./artifacts/screen-power).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/screen-power}
source "$(dirname "$0")/lib.sh"
boot_headless 2

export WAYLAND_DEBUG=client
with_noctalia '
  mark() { wc -l < "$OUT/umbriel.log"; }
  wait_log() {
    local from=$1 pattern=$2 count=$3
    for _ in $(seq 50); do
      (( $(tail -n +"$((from + 1))" "$OUT/umbriel.log" | grep -c "$pattern") >= count )) && return 0
      sleep 0.1
    done
    echo "FAIL: expected $count lines matching \"$pattern\""
    exit 1
  }
  from=$(mark)
  "$NOCTALIA" msg dpms-off > /dev/null 2>&1
  wait_log "$from" "powered off" 2
  from=$(mark)
  "$NOCTALIA" msg dpms-on > /dev/null 2>&1
  wait_log "$from" "applied mode" 2
'
requests=$(grep -c 'zwlr_output_power_v1#[0-9]*.set_mode' "$OUT/noctalia.log" || true)
echo "set_mode requests from the shell: $requests" | tee "$OUT/power.txt"
((requests >= 4)) || { echo "FAIL: the shell did not use wlr-output-power-management"; exit 1; }
echo "PASS: both outputs powered off and on through the protocol; artifacts: $OUT"
