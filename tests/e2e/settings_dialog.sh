#!/usr/bin/env bash
# Settings modals (rulebook 9.10) in light and dark: the export dialog and an alert. A second settings-open with
# NOCTALIA_SETTINGS_TEST_MODAL set opens the modal (headless runs have no pointer for the actions menu).
# Fails when a modal does not paint (frame equals the one before) or Escape does not close it.
# Writes before/<modal>-<mode>/closed screenshots to $OUT (default ./artifacts/settings-dialog).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-dialog}
source "$(dirname "$0")/lib.sh"
boot_headless 1

for modal in export alert; do
  NOCTALIA_SETTINGS_TEST_MODAL=$modal with_noctalia '
    shot() { sleep 0.8; grim "$OUT/$1.png"; }
    # One keyboard for the whole run: a wtype per key adds and removes the device, and the shell drops those keys.
    wtype -s 3600000 &
    sleep 0.3
    for mode in light dark; do
      "$NOCTALIA" msg theme-mode-set "$mode" > /dev/null
      "$NOCTALIA" msg settings-open about > /dev/null; shot "before-$NOCTALIA_SETTINGS_TEST_MODAL-$mode"
      "$NOCTALIA" msg settings-open about > /dev/null; shot "$NOCTALIA_SETTINGS_TEST_MODAL-$mode"
      cmp -s "$OUT/before-$NOCTALIA_SETTINGS_TEST_MODAL-$mode.png" "$OUT/$NOCTALIA_SETTINGS_TEST_MODAL-$mode.png" \
        && { echo "FAIL: no $NOCTALIA_SETTINGS_TEST_MODAL modal in $mode" >&2; exit 1; }
      wtype -k Escape; shot "closed-$NOCTALIA_SETTINGS_TEST_MODAL-$mode"
      cmp -s "$OUT/$NOCTALIA_SETTINGS_TEST_MODAL-$mode.png" "$OUT/closed-$NOCTALIA_SETTINGS_TEST_MODAL-$mode.png" \
        && { echo "FAIL: Escape kept the $NOCTALIA_SETTINGS_TEST_MODAL modal in $mode" >&2; exit 1; }
      "$NOCTALIA" msg settings-close > /dev/null || true
    done
  '
done
echo PASS
