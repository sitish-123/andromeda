#!/usr/bin/env bash
# Captures every first-login setup-wizard page and state for comparison with .agent/mockups/setup-wizard.html.
# Usage: SCHEME=light|dark UI_SCALE=1|1.25 tests/e2e/setup_wizard_shots.sh. Captures in $OUT/live-<page>-<scheme>[-scale<s>].png.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/artifacts/setup-wizard}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/noctalia}
SCHEME=${SCHEME:-light}
UI_SCALE=${UI_SCALE:-1}
source "$(dirname "$0")/lib.sh"

boot_headless 1
printf '\n[appearance]\nscheme = "%s"\n' "$SCHEME" >> "$RUNTIME/andromeda.toml"
printf '\n[theme]\nmode = "%s"\n\n[accessibility]\nui_scale = %s\n' "$SCHEME" "$UI_SCALE" >> "$RUNTIME/home/.config/noctalia/config.toml"
SUFFIX=$SCHEME
[[ $UI_SCALE == 1 ]] || SUFFIX=$SCHEME-scale$UI_SCALE
export SUFFIX
with_noctalia '
  shot() {
    "$NOCTALIA" msg panel-open setup-wizard "$2" > /dev/null
    sleep 1.2
    grim "$OUT/live-$1-$SUFFIX.png"
    "$NOCTALIA" msg panel-close setup-wizard > /dev/null 2>&1 || true
    sleep 0.5
  }
  sleep 1.5
  shot welcome welcome
  shot appearance appearance
  shot phone-waiting phone:waiting
  shot phone-paired phone:paired
  shot phone-failed phone:failed
  shot phone phone:norun
  shot search search:norun
  shot search-noprogress search:noprogress
  shot update-checking update:checking
  shot update-current update:current
  shot update-available update:available
  shot update-failed update:failed
  shot done done
'
ls "$OUT"/live-*-"$SUFFIX".png
