#!/usr/bin/env bash
# Control center sheet beside a vertical bar (rulebook 9.16): with a left or a right bar the sheet opens from the status
# island, its top edge at the island's top, clamped to the output. Asserts the open panel (msg panel-state) and the card
# against the island (msg panel-rect: x y width height island-top) for each side. The status island sits at the bottom
# of a 720 px output, so the card is always clamped: it must end at the output bottom and start above the island top.
# The unclamped case (card top = island top) cannot occur on this output and is not covered. Shots go to $OUT
# (default ./artifacts/control-center-vbar).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/control-center-vbar}
if [[ -z ${SIDE:-} ]]; then
  for side in left right; do SIDE=$side OUT=$OUT/$side "$0"; done
  exit 0
fi
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
boot_headless 1
mkdir -p "$RUNTIME/home/.config/noctalia"
printf '\n[bar.default]\nposition = "%s"\n' "$SIDE" >> "$RUNTIME/home/.config/noctalia/config.toml"
export OUT SIDE

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }
  msg theme-mode-set light > /dev/null
  [[ $(msg panel-state) == none ]] || fail "a panel is open before the test: $(msg panel-state)"
  msg panel-open control-center quick > /dev/null
  wait_for "the sheet did not open: $(msg panel-state)" eval "[[ \$(msg panel-state) == \"control-center quick\" ]]"
  # The open spring has settled when two reads 0.3 s apart agree.
  previous=
  for _ in $(seq 50); do
    current=$(msg panel-rect)
    [[ $current != none && $current == "$previous" ]] && break
    previous=$current
    sleep 0.3
  done
  [[ $current == "$previous" ]] || fail "the sheet position did not settle: $previous / $current"
  read -r x y w h island <<< "$current"
  grim "$OUT/vbar-$SIDE.png"
  echo "$SIDE sheet: $x $y $w $h island top $island" >> "$OUT/steps.txt"
  (( x > 0 && x + w < 1280 )) || fail "the sheet leaves the output sideways: x=$x w=$w"
  (( y >= 8 && y + h <= 720 - 8 )) || fail "the sheet leaves the output: y=$y h=$h"
  (( y < island )) || fail "a clamped card must start above the island top: y=$y island=$island"
  (( y + h >= 720 - 8 - 2 )) || fail "a clamped card must end at the output bottom: y=$y h=$h"
'
echo "PASS; artifacts: $OUT"
