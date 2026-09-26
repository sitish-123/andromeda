#!/usr/bin/env bash
# Drives the Super+P display-mode panel through every mode on a headless Umbriel with two outputs, through the same
# panel-open contexts a keybind uses, and records the compositor's state after each (modes.txt), plus a screenshot of
# the panel (panel.png). Artifacts go to $OUT (default ./artifacts/display-mode).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/display-mode}
source "$(dirname "$0")/lib.sh"
boot_headless 2

with_noctalia '
  state() {
    local mirror enabled1 enabled2 x2
    mirror=$("$DESKTOP_CLIENT" state | awk "\$1 == \"HEADLESS-2\" {print \$2}")
    read -r enabled1 enabled2 x2 < <("$UMBRIEL" outputs --json | jq -r "
      [(.[] | select(.name == \"HEADLESS-1\") | .enabled),
       (.[] | select(.name == \"HEADLESS-2\") | .enabled),
       (.[] | select(.name == \"HEADLESS-2\") | .position.x)] | @tsv")
    echo "h1=$enabled1 h2=$enabled2 h2x=$x2 mirror=$mirror"
  }
  expect() {
    local mode=$1 want=$2 got=
    "$NOCTALIA" msg panel-open display-mode "$mode" > /dev/null
    for _ in $(seq 50); do
      got=$(state)
      [[ $got == *"$want"* ]] && { echo "PASS $mode: $got" | tee -a "$OUT/modes.txt"; return 0; }
      sleep 0.1
    done
    echo "FAIL $mode: wanted $want, got $got" | tee -a "$OUT/modes.txt"
    return 1
  }
  : > "$OUT/modes.txt"
  "$NOCTALIA" msg panel-open display-mode > /dev/null
  sleep 1.5 # real time: the panel maps and paints
  grim "$OUT/panel.png"
  "$NOCTALIA" msg panel-close > /dev/null
  expect duplicate "h1=true h2=true h2x=0 mirror=HEADLESS-1"
  expect extend "h1=true h2=true h2x=1280 mirror=-"
  expect external-only "h1=false h2=true"
  expect internal-only "h1=true h2=false"
  expect duplicate "mirror=HEADLESS-1"
  expect internal-only "h1=true h2=false h2x=0 mirror=-"
'
! grep -q '^FAIL' "$OUT/modes.txt"
echo "artifacts: $OUT"
