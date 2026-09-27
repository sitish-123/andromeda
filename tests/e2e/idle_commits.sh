#!/usr/bin/env bash
# Counts compositor commits over 30 s of a static desktop: a headless Umbriel with the Noctalia fork showing a bar.
# The window starts after a minute tick and ends before the next, so a static screen must not commit at all, and panel
# self-refresh stays on. WINDOW is at most 57 s. Two bars are checked:
# a clock-only top bar, and a left (vertical), fractionally scaled bar with the clock and a capsule group of
# system-monitor gauges, the shape where gauges once fought their capsule's layout every second. The gauges show disk
# usage, which does not move within the window. BAR_START adds widgets to the top bar for manual runs on an idle
# host. Writes commits.txt and one screenshot per bar to $OUT (default ./artifacts/idle-commits).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/idle-commits}
BAR_START=${BAR_START-'"clock"'}
WINDOW=${WINDOW:-30}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
: > "$OUT/commits.txt"

measure() {
  local name=$1
  with_noctalia '
    commits() {
      local n
      n=$("$UMBRIEL" output-commits --json | jq -r ".\"HEADLESS-1\" // .ok.\"HEADLESS-1\"")
      [[ $n =~ ^[0-9]+$ ]] || { echo "no commit count: $n" >&2; exit 1; }
      echo "$n"
    }
    sleep 5 # real time: the bar maps, paints, and takes its first samples
    grim "$OUT/'"$name"'.png"
    # A window that ends before the next minute holds no clock tick, so the static screen owes zero commits.
    while (( 10#$(date +%S) < 1 || 10#$(date +%S) + '"$WINDOW"' > 58 )); do sleep 0.5; done
    before=$(commits)
    sleep '"$WINDOW"' # real time: the measurement window
    echo "'"$name"' window_s='"$WINDOW"' commits=$(($(commits) - before))" | tee -a "$OUT/commits.txt"
  '
}

run_bar() {
  local name=$1 config=$2
  (
    boot_headless 1
    printf '%s\n' "$config" >> "$RUNTIME/home/.config/noctalia/config.toml"
    measure "$name"
  )
}

run_bar top-clock "
[bar.default]
start = [ $BAR_START ]
center = []
end = []"

run_bar left-capsule '
[accessibility]
ui_scale = 1.20

[bar.default]
position = "left"
scale = 1.10
thickness = 35
start = [ "clock", "group:g1" ]
center = []
end = []

    [[bar.default.capsule_group]]
    id = "g1"
    members = [ "sysmon_2", "sysmon_3" ]
    padding = 6.0

[widget.sysmon_2]
type = "sysmon"
stat = "disk_used_pct"
show_value = false

[widget.sysmon_3]
type = "sysmon"
stat = "disk_used_pct"
show_value = false'

failed=0
while read -r name _ count; do
  count=${count#commits=}
  ((count == 0)) || { echo "FAIL: $name committed $count times on a static screen"; failed=1; }
done < "$OUT/commits.txt"
((failed == 0)) && echo "PASS; artifacts: $OUT"
exit "$failed"
