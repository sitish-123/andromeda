#!/usr/bin/env bash
# On-CPU profile of one warm shell start to its first frame, in a headless Andromeda (user-space samples only).
# Results: $OUT/report.txt (top symbols, self time) and $OUT/perf.data. Env: NOCTALIA (absolute path), ANDROMEDA.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-release/noctalia}
OUT=${OUT:-/tmp/profile-start}
source "$ROOT/tests/e2e/lib.sh"
mkdir -p "$OUT"
boot_headless 1
run env NOCTALIA="$NOCTALIA" OUT="$OUT" dbus-run-session --config-file="$RUNTIME/bus.conf" -- bash -c '
  set -euo pipefail
  export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
  first_frame() { for _ in $(seq 400); do grep -q "first frame at" "$1" && return 0; sleep 0.05; done; return 1; }
  # Warm-up start: fills the Mesa, font and icon caches.
  "$NOCTALIA" > "$OUT/warmup.log" 2>&1 & pid=$!
  first_frame "$OUT/warmup.log"; kill $pid; wait $pid || true
  perf record -F 10000 -g -o "$OUT/perf.data" -- "$NOCTALIA" > "$OUT/shell.log" 2>&1 & perf=$!
  first_frame "$OUT/shell.log"
  kill -INT $perf; wait $perf || true
'
grep -o "first frame at.*" "$OUT/shell.log" | tee "$OUT/report.txt"
perf report -i "$OUT/perf.data" --no-children --sort dso,sym --stdio 2>/dev/null | grep -v "^#" | grep -v "^$" \
  | head -60 >> "$OUT/report.txt"
perf report -i "$OUT/perf.data" --no-children --sort dso --stdio 2>/dev/null | grep -v "^#" | grep -v "^$" \
  | head -20 >> "$OUT/report.txt"
