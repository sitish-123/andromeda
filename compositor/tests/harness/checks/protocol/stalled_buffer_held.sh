#!/usr/bin/env bash
# A buffer whose GPU write never finishes (a hung video decode) stays off screen until its fence signals. Shown early,
# the render waits on the fence, and the screen and cursor freeze until the GPU resets.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_SYNCOBJ_CLIENT:-./build-debug/tests/syncobj-client}"

for mode in stalled-acquire stalled-implicit; do
  log="$ANDROMEDA_RUNTIME_DIR/syncobj-$mode.log"
  if ! "$CLIENT" "$mode" > "$log" 2>&1; then
    echo "$mode: $(cat "$log")"
    exit 1
  fi
  if grep -q '^skip$' "$log"; then
    echo "SKIP: needs vgem (modprobe vgem) and access to its /dev/dri/card node"
    exit 0
  fi
  if ! grep -q '^ok$' "$log"; then
    echo "$mode: no result: $(cat "$log")"
    exit 1
  fi
done

echo "buffers with an unfinished GPU write stay off screen until the write finishes, with and without an acquire point"
