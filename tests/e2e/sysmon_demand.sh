#!/usr/bin/env bash
# The system monitor samples only while something shows its stats: a clock-only bar never starts the sampler, a bar
# with a CPU sysmon widget does. The sampler logs "detected stats sources" when it starts. Writes result.txt to $OUT
# (default ./artifacts/sysmon-demand).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/sysmon-demand}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"; : > "$OUT/result.txt"

run_bar() {
  local name=$1 widgets=$2
  (
    boot_headless 1
    printf '\n[bar.default]\nstart = [ %s ]\ncenter = []\nend = []\n\n[widget.sysmon_cpu]\ntype = "sysmon"\nstat = "cpu_usage"\n' \
      "$widgets" >> "$RUNTIME/home/.config/noctalia/config.toml"
    with_noctalia 'sleep 3' # real time: the bar maps and its widgets are built
    cp "$OUT/noctalia.log" "$OUT/$name.log"
    echo "$name sampler_started=$(grep -c "detected stats sources" "$OUT/$name.log" || true)" >> "$OUT/result.txt"
  )
}
run_bar clock '"clock"'
run_bar sysmon '"clock", "sysmon_cpu"'
cat "$OUT/result.txt"
grep -qx "clock sampler_started=0" "$OUT/result.txt" || { echo "FAIL: sampler ran with nothing showing stats"; exit 1; }
grep -qx "sysmon sampler_started=1" "$OUT/result.txt" || { echo "FAIL: sampler did not start for the sysmon widget"; exit 1; }
echo "PASS; artifacts: $OUT"
