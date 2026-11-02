#!/usr/bin/env bash
# Launch-time baseline (docs/performance.md): shell start, launcher open and app start, in a headless Andromeda with
# a private HOME and bus. Results: $OUT/results.txt. Env: ANDROMEDA, NOCTALIA (absolute paths), RUNS, APPS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/noctalia}
OUT=${OUT:-/tmp/measure-launch}
source "$ROOT/tests/e2e/lib.sh"
mkdir -p "$OUT"
boot_headless 1
run env ANDROMEDA="$ANDROMEDA" NOCTALIA="$NOCTALIA" OUT="$OUT" RUNS="${RUNS:-5}" APPS="${APPS:-foot gnome-calculator dolphin}" \
  dbus-run-session --config-file="$RUNTIME/bus.conf" -- \
  bash -c 'export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS; exec python3 -I "$0"' "$ROOT/tools/measure-launch.py"
