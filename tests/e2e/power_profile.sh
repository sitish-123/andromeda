#!/usr/bin/env bash
# The shell switches the power-profiles-daemon profile when the machine moves between AC and battery
# ([battery] profile_on_ac / profile_on_battery), with mock UPower and power-profiles services on the test's private
# system bus. Writes profiles.txt (each ActiveProfile write) to $OUT (default ./artifacts/power-profile).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/power-profile}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/profiles.txt"
printf '\n[battery]\nprofile_on_ac = "balanced"\nprofile_on_battery = "power-saver"\n' \
  >> "$RUNTIME/home/.config/noctalia/config.toml"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_power.py"
# The mock must own its names before the shell starts, so it runs on the bus the shell will use.
with_noctalia '
  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/profiles.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  shell=$!
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  flip() {
    busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call org.freedesktop.UPower /org/freedesktop/UPower \
      dsk.test.Power SetOnBattery b "$1" > /dev/null
  }
  expect() {
    for _ in $(seq 50); do [[ $(tail -1 "$OUT/profiles.txt") == "$1" ]] && return 0; sleep 0.1; done
    echo "FAIL: expected $1, writes: $(tr "\n" "," < "$OUT/profiles.txt")"
    exit 1
  }
  flip true
  expect "ActiveProfile=power-saver"
  flip false
  expect "ActiveProfile=balanced"
  kill "$mock" "$shell"
'
echo "PASS: $(tr '\n' ',' < "$OUT/profiles.txt"); artifacts: $OUT"
