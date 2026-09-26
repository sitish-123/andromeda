#!/usr/bin/env bash
# Usage: measure-idle.sh <process-name> [seconds=30] [label]
# Prints one TSV row: label, process, seconds, rss_kb, anon_kb, threads, cpu_permille, wakeups_per_s, battery_mw.
# battery_mw is the mean of BAT*/power_now over the window, or "ac" when not discharging.
set -euo pipefail

name=${1:?process name}
secs=${2:-30}
label=${3:-$(date -Iseconds)}
pid=$(pgrep -xo "$name") || { echo "no process named $name" >&2; exit 1; }
tck=$(getconf CLK_TCK)
bat=$(ls -d /sys/class/power_supply/BAT* 2>/dev/null | head -1)

cpu() { awk '{print $14 + $15}' "/proc/$pid/stat"; }
wakeups() { cat /proc/"$pid"/task/*/status | awk '/^voluntary_ctxt_switches/ {s += $2} END {print s}'; }

c0=$(cpu); w0=$(wakeups)
mw=ac
if [[ -n $bat && $(<"$bat/status") == Discharging ]]; then
  sum=0
  for ((i = 0; i < secs; i++)); do
    sleep 1
    sum=$((sum + $(<"$bat/power_now")))
  done
  mw=$((sum / secs / 1000))
else
  sleep "$secs"
fi
c1=$(cpu); w1=$(wakeups)

read -r rss anon threads < <(awk '/^VmRSS/ {r=$2} /^RssAnon/ {a=$2} /^Threads/ {t=$2} END {print r, a, t}' "/proc/$pid/status")
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$label" "$name" "$secs" "$rss" "$anon" "$threads" \
  $(((c1 - c0) * 1000 / (secs * tck))) $(((w1 - w0) / secs)) "$mw"
