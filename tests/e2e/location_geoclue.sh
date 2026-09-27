#!/usr/bin/env bash
# Auto-locate goes through GeoClue when it is on the system bus: with a stand-in GeoClue on the test bus, turning
# auto_locate on makes the shell ask for a client, name itself and a city-level accuracy, start it, and adopt the
# location GeoClue reports, which location-status then prints as the system location. No IP lookup is made. Turning
# auto_locate off stops the client. Writes geoclue.log, steps.txt, and noctalia.log to $OUT (default
# ./artifacts/location-geoclue).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/location-geoclue}
source "$(dirname "$0")/lib.sh"
boot_headless 1
rm -f "$OUT/geoclue.log" "$OUT/steps.txt"

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 100); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  CONFIG=$RUNTIME/home/.config/noctalia/config.toml

  python3 '"$ROOT"'/tests/e2e/mock_geoclue.py "$OUT/geoclue.log" &
  wait_for "the stand-in GeoClue never came up" grep -q ready "$OUT/geoclue.log"
  [[ $(msg location-status) == none ]] || fail "a location before auto-locate was on: $(msg location-status)"

  printf "\n[location]\nauto_locate = true\n" >> "$CONFIG"
  wait_for "no location from GeoClue: $(msg location-status)" eval "[[ \$(msg location-status) == \"27.7172 85.3240 System location: Kathmandu\" ]]"
  step "resolved: $(msg location-status)"
  grep -q "set DesktopId=dev.noctalia.Noctalia" "$OUT/geoclue.log" || fail "the shell did not name itself to GeoClue"
  grep -q "set RequestedAccuracyLevel=4" "$OUT/geoclue.log" || fail "the shell did not ask for city accuracy"
  grep -q "call org.freedesktop.GeoClue2.Client.Start" "$OUT/geoclue.log" || fail "the shell did not start its client"
  ! grep -q "api.noctalia.dev" "$OUT/noctalia.log" || fail "an IP lookup was made while GeoClue was available"
  step "asked GeoClue with DesktopId and city accuracy; no IP lookup"

  sed -i "s/^auto_locate = true/auto_locate = false/" "$CONFIG"
  wait_for "auto-locate off did not stop the GeoClue client" grep -q "call org.freedesktop.GeoClue2.Client.Stop" "$OUT/geoclue.log"
  step "stopped when auto-locate was turned off"
'
cat "$OUT/steps.txt"
echo "PASS; artifacts: $OUT"
