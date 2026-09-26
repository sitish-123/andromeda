#!/usr/bin/env bash
# Counts compositor commits over 30 s of a static desktop: a headless Umbriel with the Noctalia fork showing a
# clock-only bar. A static screen must commit at most once (the minute tick), so panel self-refresh stays on.
# BAR_START swaps the bar contents; gauges read live host sensors, so a gauge bar is only meaningful on an idle host
# (BAR_START='"clock", "temp", "sysmon_2", "sysmon_3"'). Writes commits.txt and bar.png to $OUT (default
# ./artifacts/idle-commits).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/idle-commits}
BAR_START=${BAR_START-'"clock"'}
source "$(dirname "$0")/lib.sh"
boot_headless 1

cat >> "$RUNTIME/home/.config/noctalia/config.toml" <<EOF

[bar.default]
start = [ $BAR_START ]
center = []
end = []

[widget.temp]
show_value = false

[widget.sysmon_2]
type = "sysmon"
stat = "gpu_temp"
show_value = false

[widget.sysmon_3]
type = "sysmon"
stat = "ram_used"
show_value = false
EOF

WINDOW=${WINDOW:-30}
with_noctalia '
  commits() {
    local n
    n=$("$UMBRIEL" output-commits --json | jq -r ".\"HEADLESS-1\" // .ok.\"HEADLESS-1\"")
    [[ $n =~ ^[0-9]+$ ]] || { echo "no commit count: $n" >&2; exit 1; }
    echo "$n"
  }
  sleep 5 # real time: the bar maps, paints, and takes its first samples
  grim "$OUT/bar.png"
  before=$(commits)
  sleep '"$WINDOW"' # real time: the measurement window
  after=$(commits)
  echo "window_s='"$WINDOW"' commits=$((after - before))" | tee "$OUT/commits.txt"
'
read -r commits < <(sed -n 's/.*commits=//p' "$OUT/commits.txt")
((commits <= 1)) || { echo "FAIL: $commits commits on a static screen" >&2; exit 1; }
echo "PASS: $commits commits in ${WINDOW} s; artifacts: $OUT"
