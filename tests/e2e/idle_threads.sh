#!/usr/bin/env bash
# Counts the shell's threads on an idle desktop (a headless Umbriel, a clock-only bar), 5 s after it answers IPC and
# again 40 s later, once on-demand workers have had time to exit. Named pools (disk I/O, render) grow and shrink on
# their own, so the check is on unnamed "noctalia" threads, the always-on workers: at most MAX_WORKERS. Writes
# threads.txt (counts and per-name tally) to $OUT (default ./artifacts/idle-threads).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/idle-threads}
MAX_WORKERS=${MAX_WORKERS:-4}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  count() { awk "/^Threads:/ {print \$2}" /proc/$pid/status; }
  sleep 5; early=$(count)
  sleep 40; late=$(count) # real time: past the 30 s idle exit of on-demand workers
  { echo "early=$early late=$late"; cat /proc/$pid/task/*/comm | sort | uniq -c | sort -rn; } > "$OUT/threads.txt"
'
cat "$OUT/threads.txt"
workers=$(awk '$2 == "noctalia" {print $1}' "$OUT/threads.txt")
(( workers <= MAX_WORKERS )) || { echo "FAIL: $workers unnamed worker threads idle > $MAX_WORKERS"; exit 1; }
echo "PASS; artifacts: $OUT"
