#!/usr/bin/env bash
# The Wi-Fi hotspot goes through NetworkManager (a mock on the private system bus records calls): the first
# `noctalia msg hotspot-toggle` creates an access-point profile with shared IPv4 and a 12-character key, the next
# deactivates it, and a third reuses the saved profile instead of adding another; hotspot-status follows each step.
# Writes calls.txt and status.txt to $OUT (default ./artifacts/hotspot).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/hotspot}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_nm.py"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  step() {
    "$NOCTALIA" msg hotspot-toggle > /dev/null
    for _ in $(seq 30); do [[ $("$NOCTALIA" msg hotspot-status) == "$1" ]] && break; sleep 0.1; done
    echo "$("$NOCTALIA" msg hotspot-status)" >> "$OUT/status.txt"
  }
  echo "$("$NOCTALIA" msg hotspot-status)" > "$OUT/status.txt"
  step on; step off; step on
  kill %2
'
cat "$OUT/calls.txt"
[[ $(tr "\n" " " < "$OUT/status.txt") == "off on off on " ]] || { echo "FAIL: status $(tr "\n" " " < "$OUT/status.txt")"; exit 1; }
grep -qx "Add mode=ap ipv4=shared psk_len=12 device=/org/freedesktop/NetworkManager/Devices/1" "$OUT/calls.txt" \
  || { echo "FAIL: no AP profile added"; exit 1; }
grep -qx "Deactivate /org/freedesktop/NetworkManager/ActiveConnection/1" "$OUT/calls.txt" \
  || { echo "FAIL: not deactivated"; exit 1; }
grep -qx "Activate /org/freedesktop/NetworkManager/Settings/1" "$OUT/calls.txt" \
  || { echo "FAIL: saved profile not reused"; exit 1; }
[[ $(grep -c "^Add" "$OUT/calls.txt") == 1 ]] || { echo "FAIL: profile added twice"; exit 1; }
echo "PASS; artifacts: $OUT"
