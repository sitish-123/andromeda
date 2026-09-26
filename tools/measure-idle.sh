#!/usr/bin/env bash
# Usage: [sudo] measure-idle.sh [seconds=60] [label] [process...=umbriel noctalia]
# Prints one TSV row per process: label, process, seconds, rss_kb, anon_kb, threads, cpu_permille, wakeups_per_s,
# then whole-machine columns (same on every row): battery_mw, pkg_w, deep_idle, psr_active_pct.
# battery_mw is "ac" unless discharging. deep_idle is the deepest package C-state residency turbostat can read on
# this CPU, as "<counter>=<percent>" (SYS%LPI when no package counter exists). pkg_w and deep_idle need root and
# turbostat, psr_active_pct needs root (debugfs); without them they read "-".
set -euo pipefail

secs=${1:-60}
label=${2:-$(date -Iseconds)}
shift $(($# < 2 ? $# : 2))
if (($#)); then procs=("$@"); else procs=(umbriel noctalia); fi
tck=$(getconf CLK_TCK)
bat=$(ls -d /sys/class/power_supply/BAT* 2>/dev/null | head -1)
psr=$(find /sys/kernel/debug/dri -name i915_edp_psr_status -o -name i915_psr_status 2>/dev/null | head -1 || true)
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

declare -A pid cpu0 wake0
cpu() { awk '{print $14 + $15}' "/proc/$1/stat"; }
wakeups() { cat /proc/"$1"/task/*/status | awk '/^voluntary_ctxt_switches/ {s += $2} END {print s}'; }
for p in "${procs[@]}"; do
  pid[$p]=$(pgrep -xo "$p") || { echo "no process named $p" >&2; exit 1; }
  cpu0[$p]=$(cpu "${pid[$p]}")
  wake0[$p]=$(wakeups "${pid[$p]}")
done

tpid=
if ((EUID == 0)) && command -v turbostat > /dev/null; then
  turbostat --quiet --Summary --interval "$secs" --num_iterations 1 --out "$tmp" &
  tpid=$!
fi

mw=ac
[[ -n $bat && $(<"$bat/status") == Discharging ]] && mw=0
psr_on=0
for ((i = 0; i < secs; i++)); do
  sleep 1
  [[ $mw != ac ]] && mw=$((mw + $(<"$bat/power_now") / 1000))
  # "Source PSR/PanelReplay status: <state> [0x...]"; self-refresh is SRDENT (PSR1) or *SLEEP (PSR2).
  [[ -n $psr ]] && grep -qE 'status: (SRDENT|SLEEP|FAST_SLEEP|DEEP_SLEEP)' "$psr" && psr_on=$((psr_on + 1))
done
[[ $mw != ac ]] && mw=$((mw / secs))

pkg_w=- deep=-
if [[ -n $tpid ]]; then
  wait "$tpid" || true
  read -r pkg_w deep < <(awk '
    NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i }
    NR == 2 {
      d = "-"
      n = split("Pkg%pc10 Pkg%pc8 Pkg%pc6 Pkg%pc3 Pkg%pc2 SYS%LPI", c, " ")
      for (i = 1; i <= n; i++) if (c[i] in h) { d = c[i] "=" $h[c[i]]; break }
      print ("PkgWatt" in h ? $h["PkgWatt"] : "-"), d
    }' "$tmp") || true
fi
psr_pct=-
[[ -n $psr ]] && psr_pct=$((psr_on * 100 / secs))

for p in "${procs[@]}"; do
  read -r rss anon threads < <(awk '/^VmRSS/ {r=$2} /^RssAnon/ {a=$2} /^Threads/ {t=$2} END {print r, a, t}' \
    "/proc/${pid[$p]}/status")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$p" "$secs" "$rss" "$anon" "$threads" \
    $((($(cpu "${pid[$p]}") - cpu0[$p]) * 1000 / (secs * tck))) \
    $((($(wakeups "${pid[$p]}") - wake0[$p]) / secs)) "$mw" "$pkg_w" "$deep" "$psr_pct"
done
