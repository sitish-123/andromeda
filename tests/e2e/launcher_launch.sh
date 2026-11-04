#!/usr/bin/env bash
# An app launch from the launcher: Enter on "foot" opens a foot window. The launcher takes its activation token from
# the token's done event and names the row icon with dsk_surface_v1.launch_from, so frames taken while foot starts
# show the icon moving away from the launcher (rulebook P15). Fails when no foot window maps within 5 s.
# Writes launch-*.png (every 80 ms from Enter) and noctalia.log to $OUT (default ./artifacts/launcher-launch).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/launcher-launch}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  windows() { "$ANDROMEDA" windows --json | jq "[.[] | select(.app_id == \"foot\")] | length"; }
  wtype -s 3600000 &
  "$NOCTALIA" msg panel-open launcher foot > /dev/null
  wait_for "the launcher did not draw" "grep -q \"open to first frame\" \"$OUT/noctalia.log\""
  wtype -k Return
  for i in 1 2 3 4 5 6; do grim "$OUT/launch-$i.png"; sleep 0.08; done
  wait_for "Enter on foot opened no foot window" "(( \$(windows) >= 1 ))"
'
echo "PASS; artifacts: $OUT"
