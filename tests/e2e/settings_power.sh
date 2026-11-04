#!/usr/bin/env bash
# Settings › Power "Lid & Power Button" card reads logind: with a mock logind reporting hibernate on the lid and
# CanHibernate=no (or MOCK_CAN_HIBERNATE=yes), the page shows the raw value and no Hibernate choice before a trial.
# Writes light/dark screenshots to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-power}
source "$(dirname "$0")/lib.sh"
boot_headless 1
# UI_SCALE=0.75 fits the whole page in the 720 px headless output, so one shot shows every card.
[ -z "${UI_SCALE:-}" ] || printf '\n[accessibility]\nui_scale = %s\n' "$UI_SCALE" >> "$RUNTIME/home/.config/noctalia/config.toml"
: > "$OUT/calls.txt"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
export MOCK_LID_BATTERY=hibernate MOCK_CAN_HIBERNATE=${MOCK_CAN_HIBERNATE:-no} MOCK_CAN_SUSPEND_THEN_HIBERNATE=no
BEFORE_NOCTALIA='
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock logind did not start"; exit 1; }
' with_noctalia '
  for mode in light dark; do
    "$NOCTALIA" msg theme-mode-set "$mode" > /dev/null
    "$NOCTALIA" msg settings-open power > /dev/null
    sleep 1
    grim "$OUT/power-$mode-hibernate-$MOCK_CAN_HIBERNATE.png"
  done
'
grep -q "power-config" "$OUT/noctalia.log" && { echo "FAIL: power-config warnings:"; grep power-config "$OUT/noctalia.log"; exit 1; }
echo PASS
