#!/usr/bin/env bash
# Minimize is offered to a window only while it can be minimized.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/wm-capabilities-client.log"

if [[ ! -x $CLIENT ]]; then
  echo "unmap-client is not built"
  exit 1
fi

# Waits for capabilities event number $1 and asserts what it carries; $2 is whether minimize is expected.
expect_block() {
  local count=$1 minimize=$2 block
  await_lines "$CLIENT_LOG" 'wm-capabilities-done$' "$count"
  block=$(awk -v want="$count" '
    /^wm-capabilities-done$/ { if (++seen == want) { printf "%s", block; exit } block = ""; next }
    { block = block $0 "\n" }' "$CLIENT_LOG")
  for capability in window-menu maximize fullscreen; do
    if ! grep -q "^wm-capability=$capability$" <<< "$block"; then
      echo "event $count lacks $capability: $(< "$CLIENT_LOG")"
      exit 1
    fi
  done
  if [[ $minimize == true ]] && ! grep -q '^wm-capability=minimize$' <<< "$block"; then
    echo "event $count does not offer minimize to a floating window: $(< "$CLIENT_LOG")"
    exit 1
  fi
  if [[ $minimize == false ]] && grep -q '^wm-capability=minimize$' <<< "$block"; then
    echo "event $count offers minimize to a tiled window: $(< "$CLIENT_LOG")"
    exit 1
  fi
}

LOG_WM_CAPABILITIES=1 "$CLIENT" wm-capabilities > "$CLIENT_LOG" 2>&1 &

# The default layout tiles the window: no minimize.
expect_block 1 false
await_lines "$CLIENT_LOG" 'mapped$' 1

"$ANDROMEDA" msg window-toggle-floating > /dev/null
expect_block 2 true

"$ANDROMEDA" msg window-toggle-floating > /dev/null
expect_block 3 false

echo "minimize is offered while the window floats and withdrawn when it tiles again"
