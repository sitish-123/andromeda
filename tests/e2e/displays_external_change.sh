#!/usr/bin/env bash
# Settings > Displays must follow a layout change it did not make (a hotplug, another settings client): the server's
# new state replaces edits the user never touched, so Apply stays disabled instead of offering to revert the change.
# Opens the page with two outputs, moves HEADLESS-2 below HEADLESS-1 through output management, scrolls to the Apply
# button, and counts its enabled (primary yellow) pixels. Writes result.txt, before.png, and after.png to $OUT
# (default ./artifacts/displays-external-change).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/displays-external-change}
source "$(dirname "$0")/lib.sh"
boot_headless 2
export OM=$ROOT/compositor/build-debug/tests/output-management-client
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
PROBE=$ROOT/compositor/build-debug/tests/pixel-probe
with_noctalia '
  scroll_down() { "$POINTER" "$1" "$2" move "$3" "$4" $(printf "notch 1 %.0s" $(seq 20)) > /dev/null; sleep 1; }
  "$NOCTALIA" msg settings-open displays > /dev/null
  sleep 2 # real time: the settings window maps and paints
  scroll_down 2560 720 742 400
  grim -o HEADLESS-2 "$OUT/before.png"
  "$OM" apply enable HEADLESS-2 0 720 > /dev/null
  sleep 1.5 # real time: the page rebuilds from the new state
  scroll_down 2560 1440 742 1152
  grim -o HEADLESS-2 "$OUT/after.png"
  "$UMBRIEL" outputs --json > "$OUT/outputs.json"
'
# The page is scrolled to the bottom, where Apply is the only primary-coloured control in this corner.
BUTTONS='400x100+880+620'
LIT='r > 0.9 && g > 0.9 && b < 0.7'
moved=$(jq -r '.[] | select(.name == "HEADLESS-2") | "\(.position.x),\(.position.y)"' "$OUT/outputs.json")
before=$("$PROBE" "$OUT/before.png" count "$LIT" "$BUTTONS")
after=$("$PROBE" "$OUT/after.png" count "$LIT" "$BUTTONS")
{
  echo "HEADLESS-2 at $moved; lit Apply pixels before: $before, after: $after"
  [[ $moved == 0,720 ]] && echo "PASS the external change moved HEADLESS-2" || echo "FAIL HEADLESS-2 did not move"
  (( before < 100 )) && echo "PASS Apply is disabled before any change" || echo "FAIL Apply is enabled with no edit"
  (( after < 100 )) && echo "PASS Apply stays disabled after the external change" \
    || echo "FAIL Apply lit up with stale edits after the external change"
} | tee "$OUT/result.txt"
! grep -q '^FAIL' "$OUT/result.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
