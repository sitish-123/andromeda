#!/usr/bin/env bash
# dsk_shell_v1 end to end: a compositor `shell:` action opens a shell panel without starting a process, Caps Lock
# typed on a virtual keyboard reaches the shell as compositor lock-key state, and the idle shell's wakeups are counted
# with the lock-keys OSD enabled (it polled LEDs every 200 ms before). Writes result.txt to $OUT (default
# ./artifacts/shell-protocol).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/shell-protocol}
source "$(dirname "$0")/lib.sh"
boot_headless 1
KEYS=${KEYS:-$ROOT/compositor/build-debug/tests/pointer-client}

with_noctalia '
  layer_present() { "$UMBRIEL" layers | grep -q "$1"; }
  "$UMBRIEL" msg "shell:panel-open launcher" > /dev/null
  for _ in $(seq 50); do layer_present noctalia-panel && break; sleep 0.1; done
  layer_present noctalia-panel || { echo "FAIL: shell action did not open the launcher panel"; exit 1; }
  "$NOCTALIA" msg panel-close > /dev/null

  pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  wakeups() { cat /proc/"$pid"/task/*/status | awk "/^voluntary_ctxt_switches/ {s += \$2} END {print s}"; }
  sleep 2 # real time: let the panel close settle
  before=$(wakeups)
  sleep 5 # real time: idle measurement window
  idle=$(( ($(wakeups) - before) / 5 ))
  echo "idle shell wakeups/s: $idle" | tee "$OUT/result.txt"
  ((idle <= 10)) || { echo "FAIL: idle shell woke $idle times per second"; exit 1; }

  '"$KEYS"' 1280 720 tap 58 > /dev/null
  for _ in $(seq 50); do layer_present noctalia-osd && break; sleep 0.1; done
  layer_present noctalia-osd || { echo "FAIL: Caps Lock did not reach the shell"; exit 1; }
'
echo "PASS; artifacts: $OUT"
