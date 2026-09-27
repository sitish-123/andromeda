#!/usr/bin/env bash
# An exclusive-keyboard layer surface (a launcher, a lock-style prompt) on HEADLESS-2 holds keyboard focus. When
# HEADLESS-2 becomes a mirror of HEADLESS-1 its layer surfaces are no longer shown, so they must be closed, as when
# the output is unplugged, and focus must return to the window on HEADLESS-1 instead of staying on an invisible
# surface. Writes focus.txt, the layer client's log, and the compositor log to $OUT (default
# ./artifacts/mirror-layer-focus).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-layer-focus}
source "$(dirname "$0")/lib.sh"
boot_headless 2
LAYER=$ROOT/compositor/build-debug/tests/layer-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
: > "$OUT/focus.txt"
result() { echo "$1" >> "$OUT/focus.txt"; }
active() { run "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "keep") | .active'; }
wait_active() {
  for _ in $(seq 50); do [[ $(active) == "$1" ]] && return 0; sleep 0.1; done
  return 1
}

# Headless has no keyboard; a virtual one gives the seat keyboard focus to hand around, as a real session has.
run "$POINTER" 1 1 keyboard-only mod none mark ready pause 86400000 > "$OUT/keyboard.log" 2>&1 &
for _ in $(seq 50); do grep -q '^ready$' "$OUT/keyboard.log" && break; sleep 0.1; done
run foot --config=/dev/null --title=keep sh -c 'sleep 60' > /dev/null 2>&1 &
wait_active true && result "PASS the window on HEADLESS-1 has focus" || result "FAIL the window never took focus"
run "$LAYER" HEADLESS-2 40 keyboard=exclusive > "$OUT/layer.log" 2>&1 &
layer=$!
for _ in $(seq 50); do grep -q '^ready$' "$OUT/layer.log" && break; sleep 0.1; done
wait_active false && grep -q '^keyboard-enter$' "$OUT/layer.log" \
  && result "PASS the exclusive layer on HEADLESS-2 took keyboard focus" \
  || result "FAIL the exclusive layer did not take keyboard focus"

run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
for _ in $(seq 50); do kill -0 "$layer" 2> /dev/null || break; sleep 0.1; done
kill -0 "$layer" 2> /dev/null && result "FAIL the layer surface on the mirror was not closed" \
  || result "PASS the layer surface on the mirror was closed"
wait_active true && result "PASS focus returned to the window on HEADLESS-1" \
  || result "FAIL focus stayed on the hidden layer surface"
run "$UMBRIEL" layers > "$OUT/layers.txt" 2>&1 || true
run "$UMBRIEL" windows --json > "$OUT/windows.json"

cat "$OUT/focus.txt"
! grep -q '^FAIL' "$OUT/focus.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
