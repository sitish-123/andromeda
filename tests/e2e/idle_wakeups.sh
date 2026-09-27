#!/usr/bin/env bash
# Measures the shell's main-loop wakeups on a static desktop (a headless Umbriel, a clock-only bar) over 30 s with
# NOCTALIA_IDLE_PROFILE, and reports wakeups per second plus the poll sources behind them. Fails above MAX_WAKEUPS
# per second (default 3). BAR_START adds widgets for manual runs. Writes wakeups.txt and profile.txt to $OUT
# (default ./artifacts/idle-wakeups).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/idle-wakeups}
BAR_START=${BAR_START-'"clock"'}
MAX_WAKEUPS=${MAX_WAKEUPS:-3}
source "$(dirname "$0")/lib.sh"
boot_headless 1
printf '\n[bar.default]\nstart = [ %s ]\ncenter = []\nend = []\n' "$BAR_START" \
  >> "$RUNTIME/home/.config/noctalia/config.toml"
export NOCTALIA_IDLE_PROFILE=1

with_noctalia 'sleep 35' # real time: 5 s settle, then three 10 s profile windows

grep 'idle profile' "$OUT/noctalia.log" | sed 's/^.*idle profile/idle profile/' > "$OUT/profile.txt"
loops=$(sed -n 's/^idle profile \([0-9.]*\)s: loops=\([0-9]*\).*/\1 \2/p' "$OUT/profile.txt" | tail -2)
[[ -n $loops ]] || { echo "FAIL: no idle profile windows"; exit 1; }
rate=$(awk '{s += $1; l += $2} END {printf "%.1f", l / s}' <<< "$loops")
echo "bar=[$BAR_START] wakeups_per_s=$rate" | tee "$OUT/wakeups.txt"
grep 'idle profile source' "$OUT/profile.txt" | tail -10 | tee -a "$OUT/wakeups.txt"
awk -v r="$rate" -v m="$MAX_WAKEUPS" 'BEGIN {exit !(r <= m)}' || { echo "FAIL: $rate wakeups/s > $MAX_WAKEUPS"; exit 1; }
echo "PASS; artifacts: $OUT"
