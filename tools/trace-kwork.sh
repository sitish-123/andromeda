#!/usr/bin/env bash
# Usage: sudo trace-kwork.sh [seconds=20]
# Ranks kernel workqueue functions by CPU time over the window: total_ms, runs, mean_ms, function.
set -euo pipefail

secs=${1:-20}
t=/sys/kernel/tracing
events=(workqueue_execute_start workqueue_execute_end)
for e in "${events[@]}"; do echo 1 > "$t/events/workqueue/$e/enable"; done
echo > "$t/trace"
sleep "$secs"
for e in "${events[@]}"; do echo 0 > "$t/events/workqueue/$e/enable"; done

awk -v secs="$secs" '
  / workqueue_execute_(start|end): / {
    cpu = $2
    for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+:$/) ts = substr($i, 1, length($i) - 1)
    match($0, /work struct [0-9a-fx]+/); w = substr($0, RSTART + 12, RLENGTH - 12)
    match($0, /function [A-Za-z0-9_.]+/); f = substr($0, RSTART + 9, RLENGTH - 9)
    if ($0 ~ /execute_start/) { start[cpu w] = ts; fn[cpu w] = f }
    else if ((cpu w) in start) {
      f = fn[cpu w]; total[f] += ts - start[cpu w]; runs[f]++; delete start[cpu w]
    }
  }
  END {
    for (f in total) printf "%9.1f ms/s %7.1f runs/s %8.2f ms %s\n", total[f] * 1000 / secs, runs[f] / secs, total[f] * 1000 / runs[f], f
  }' "$t/trace" | sort -rn | head -20
echo > "$t/trace"
