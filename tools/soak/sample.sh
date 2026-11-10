#!/bin/bash
# Soak sampler, runs on the laptop as root: the compositor has cap_sys_nice, so a user cannot read its /proc files.
# Usage: sample.sh <out-dir> [interval-s]
set -u
out=${1:?out dir}
interval=${2:-30}
mkdir -p "$out"
tsv=$out/samples.tsv
tick=$(getconf CLK_TCK)
re='^(andromeda|noctalia|xdg-desktop-por|avahi-daemon|Xwayland|xwayland-sat|pipewire|wireplumber)'
declare -A pcpu pwk pname known

[[ -s $tsv ]] || printf 't\tkind\tpid\tname\tcpu_ms\twakeups\tthreads\tfds\tpss_kb\tpdirty_kb\tanon_kb\tgpu_kb\tgpu_res_kb\tpower_w\tload1\tmemavail_kb\tswap_kb\ttemp_c\tnote\n' >"$tsv"

procname() {
  local -a argv
  mapfile -d '' -t argv 2>/dev/null <"/proc/$1/cmdline" || return 1
  ((${#argv[@]})) || return 1
  local n=${argv[0]##*/}
  [[ $n == bash || $n == sh ]] && n=${argv[1]##*/}
  [[ ${argv[1]:-} == lock || ${argv[1]:-} == --session ]] && n+=-${argv[1]#--}
  printf %s "$n"
}

# Sums each DRM client once: several fds can share one drm-client-id.
gpu() {
  grep -sH '^drm-' /proc/$1/fdinfo/* | awk -F'[:\t ]+' '
    function kb(v, u) { return u == "KiB" ? v : u == "MiB" ? v * 1024 : u == "GiB" ? v * 1048576 : v / 1024 }
    { f = $1 }
    $2 == "drm-client-id" { cid[f] = $3; next }
    { key = cid[f] SUBSEP $2 }
    cid[f] != "" && !(key in seen) && ($2 ~ /^drm-(total|memory)-/) { seen[key]; tot += kb($3, $4) }
    cid[f] != "" && !(key in seen) && ($2 ~ /^drm-resident-/) { seen[key]; res += kb($3, $4) }
    END { printf "%d\t%d", tot, res }'
}

machine() {
  local now=$1 pw= st t= m
  if [[ -r /sys/class/power_supply/BAT0/power_now ]]; then
    st=$(</sys/class/power_supply/BAT0/status)
    [[ $st == Discharging ]] && pw=$(awk '{printf "%.2f", $1 / 1e6}' /sys/class/power_supply/BAT0/power_now)
  fi
  for z in /sys/class/thermal/thermal_zone*; do
    [[ $(<"$z/type") == x86_pkg_temp ]] && t=$(($(<"$z/temp") / 1000))
  done
  m=$(awk '/^MemAvailable/{a=$2} /^SwapTotal/{s=$2} /^SwapFree/{f=$2} END{printf "%d\t%d", a, s-f}' /proc/meminfo)
  printf '%s\tmachine\t\tmachine\t\t\t\t\t\t\t\t\t\t%s\t%s\t%s\t%s\t%s\n' "$now" "$pw" "$(cut -d' ' -f1 /proc/loadavg)" "$m" "$t" "${st:-}"
}

while :; do
  now=$(date +%s)
  declare -A seen=()
  {
    machine "$now"
    for d in /proc/[0-9]*; do
      p=${d#/proc/}
      read -r comm 2>/dev/null <"$d/comm" || continue
      [[ $comm =~ $re ]] || continue
      n=$(procname "$p") || continue
      [[ $n =~ ^(andromeda|noctalia|avahi-daemon|Xwayland|xwayland-satellite|pipewire|wireplumber|xdg-desktop-portal-andromeda) ]] || continue
      read -r stat 2>/dev/null <"$d/stat" || continue
      read -ra s <<<"${stat##*) }"
      cpu=$(( (s[11] + s[12]) * 1000 / tick ))
      read -r thr wk < <(cat "$d"/task/*/status 2>/dev/null | awk '/^Threads/{t=$2} /ctxt_switches/{w+=$2} END{print t, w}')
      fds=("$d"/fd/*)
      mem=$(awk '/^Pss:/{p=$2} /^Private_Dirty:/{d=$2} /^Anonymous:/{a=$2} END{printf "%d\t%d\t%d", p, d, a}' "$d/smaps_rollup" 2>/dev/null)
      g=$(gpu "$p")
      seen[$p]=1
      note=
      if [[ -z ${pname[$p]:-} ]]; then
        [[ -n ${known[$n]:-} ]] && note=restart
        known[$n]=1
        dc= dw=
      else
        dc=$((cpu - pcpu[$p])) dw=$((wk - pwk[$p]))
      fi
      pcpu[$p]=$cpu pwk[$p]=$wk pname[$p]=$n
      printf '%s\tproc\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t\t\t\t\t\t%s\n' "$now" "$p" "$n" "$dc" "$dw" "$thr" "${#fds[@]}" "$mem" "$g" "$note"
    done
    for p in "${!pname[@]}"; do
      [[ -n ${seen[$p]:-} ]] && continue
      printf '%s\tevent\t%s\t%s\t\t\t\t\t\t\t\t\t\t\t\t\t\t\texit\n' "$now" "$p" "${pname[$p]}"
      unset "pname[$p]" "pcpu[$p]" "pwk[$p]"
    done
  } >>"$tsv"
  unset seen
  sleep "$interval"
done
