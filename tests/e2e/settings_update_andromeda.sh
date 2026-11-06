#!/usr/bin/env bash
# Settings › Software Update, Andromeda card (mock_update.py, MOCK_ANDROMEDA): "Update Andromeda" works while the Arch
# news still blocks the system update, calls UpgradeAndromeda, and a refusal for newer system packages says so.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-update-andromeda}
source "$(dirname "$0")/lib.sh"
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
: > "$OUT/calls.txt"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_update.py"
export MOCK_NEWS=1 MOCK_ANDROMEDA=1
mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho "flatpak $*" >> "%s"\n' "$OUT/calls.txt" > "$OUT/bin/flatpak"
chmod +x "$OUT/bin/flatpak"
BEFORE_NOCTALIA='
  export PATH="$OUT/bin:$PATH"
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock update did not start"; exit 1; }
' with_noctalia '
  wait_calls() {  # wait_calls PATTERN COUNT: the mock logged PATTERN at least COUNT times (5 s)
    for _ in $(seq 50); do [ "$(grep -c "$1" "$OUT/calls.txt" || true)" -ge "$2" ] && return 0; sleep 0.1; done
    echo "FAIL: mock never saw $1 x$2"; cat "$OUT/calls.txt"; exit 1
  }
  "$NOCTALIA" msg settings-open software-update > /dev/null
  wait_calls GetAll 1
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  sleep 0.5
  grim "$OUT/andromeda-available-dark.png"
  # Position on the 1280x720 headless output (the screenshot shows it).
  "$POINTER" 1280 720 move 990 170 click 272 > /dev/null   # Update Andromeda
  wait_calls "^UpgradeAndromeda" 1
  sleep 1
  grim "$OUT/andromeda-needs-system-dark.png"
  grep -q "^Upgrade " "$OUT/calls.txt" && { echo "FAIL: Update Andromeda started the full upgrade"; exit 1; }
  grep -q "^flatpak" "$OUT/calls.txt" && { echo "FAIL: Update Andromeda updated apps"; exit 1; }
  true
'
grep -n "software-update" "$OUT/noctalia.log" | grep -i -E "warn|error" && { echo "FAIL: software-update warnings"; exit 1; }
echo PASS
