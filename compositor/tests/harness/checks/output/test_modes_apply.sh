#!/usr/bin/env bash
# harness: outputs=1
# A headless output given listed modes by `output-test-modes` takes a configured one as a custom mode: the headless
# backend aborts on a listed mode.
set -euo pipefail

"$ANDROMEDA" output-test-modes HEADLESS-1 1280x720@144 1280x720@60 > /dev/null

mark=$(wc -l < "$ANDROMEDA_LOG")
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[output.HEADLESS-1]
mode = "1280x720@60"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

for _ in $(seq 40); do
  if tail -n +"$((mark + 1))" "$ANDROMEDA_LOG" | grep -q "output 'HEADLESS-1': applied mode=1280x720@60000mHz"; then
    exit 0
  fi
  sleep 0.1
done
echo "the configured listed mode was not applied"
tail -n 20 "$ANDROMEDA_LOG"
exit 1
