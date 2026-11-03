#!/usr/bin/env bash
# Glass cost bench (CLAUDE.md Decisions: glass rendering plan). Runs a headless desktop per mode (fixed 1920x1080@60, so
# the parent session cannot resize or throttle it), drives fixed scenarios
# and reports per-frame GPU time (timer queries, ANDROMEDA_FRAME_LOG), frame counts, compositor CPU and RSS, package
# energy (RAPL) and GPU busy (intel_gpu_top, needs CAP_PERFMON).
# Usage: tests/glass-bench.sh [modes...]   modes: off clear tinted frosted nolens (tinted, lens off)
#   (default: off nolens tinted)
# Env: SECONDS_PER=8 (seconds per scenario), OUT=/tmp/glass-bench, BUDGET_SCALE=1.0 (this GPU's speed / UHD 620's).
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/noctalia}
SECONDS_PER=${SECONDS_PER:-8}
OUT=${OUT:-/tmp/glass-bench}
BUDGET_SCALE=${BUDGET_SCALE:-1.0}
MODES=("$@")
[[ ${#MODES[@]} -gt 0 ]] || MODES=(off nolens tinted)
mkdir -p "$OUT"
RAPL=/sys/class/powercap/intel-rapl:0/energy_uj

# The user's compositor config with the glass mode set in [appearance], no autostart, no cheatsheet.
make_config() {
  local mode=$1 dst=$2 reduce=false glass=0.5
  case $mode in
    off) reduce=true ;;
    clear) glass=0.0 ;;
    tinted | nolens) glass=0.5 ;;
    frosted) glass=1.0 ;;
    *) echo "unknown mode $mode" >&2; exit 2 ;;
  esac
  sed -e '/^autostart *=/d' -e '/^show_cheatsheet *=/d' -e '/^reduce_transparency *=/d' -e '/^glass *=/d' \
    -e '/^glass_opacity *=/d' -e '/^\[general\]/a show_cheatsheet = false' ~/.config/andromeda/config.toml > "$dst"
  if grep -q '^\[appearance\]' "$dst"; then
    sed -i "/^\[appearance\]/a reduce_transparency = $reduce\nglass = $glass" "$dst"
  else
    printf '\n[appearance]\nreduce_transparency = %s\nglass = %s\n' "$reduce" "$glass" >> "$dst"
  fi
  printf '\n[output.HEADLESS-1]\nmode = "1920x1080@60"\n' >> "$dst"
}

energy() { cat "$RAPL" 2>/dev/null || echo 0; }
cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
rss_kb() { awk '/^VmRSS/ {print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }

# p50 p99 max of GPU ns (column 4) and frame count for log lines with start time in [from, to).
frame_stats() {
  awk -v from="$2" -v to="$3" '$1 >= from && $1 < to {print $4}' "$1" | sort -n | awk '
    {v[NR] = $1} END {
      if (NR == 0) {print "0 0 0 0"; exit}
      p50 = v[int(NR * 0.50) > 0 ? int(NR * 0.50) : 1]; p99 = v[int(NR * 0.99) > 0 ? int(NR * 0.99) : 1]
      printf "%d %.3f %.3f %.3f\n", NR, p50 / 1e6, p99 / 1e6, v[NR] / 1e6}'
}

# CLOCK_MONOTONIC, the clock of the frame log.
mono_ns() { python3 -c 'import time; print(time.monotonic_ns())'; }

run_mode() {
  local mode=$1 dir="$OUT/$1"
  rm -rf "$dir"; mkdir -p "$dir"
  local R; R=$(mktemp -d /tmp/glass-bench.XXXX)
  local H=$R/home
  mkdir -p "$H/.config" "$H/.local/state"
  cp -r ~/.config/noctalia "$H/.config/" 2>/dev/null || true
  cp -r ~/.local/state/noctalia "$H/.local/state/" 2>/dev/null || true
  ln -s ~/Pictures "$H/Pictures"
  make_config "$mode" "$R/andromeda.toml"

  env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$R" WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
    ANDROMEDA_FRAME_LOG="$dir/frames.log" ANDROMEDA_LENS="$([[ $mode == nolens ]] && echo 0 || echo 1)" "$ANDROMEDA" -c "$R/andromeda.toml" > "$R/andromeda.log" 2>&1 &
  local comp=$!
  for _ in $(seq 100); do [[ -S $R/wayland-0 ]] && break; sleep 0.05; done
  [[ -S $R/wayland-0 ]] || { echo "andromeda did not start ($mode)" >&2; tail -n 20 "$R/andromeda.log" >&2; kill $comp; return 1; }

  env -u DISPLAY -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME -u XDG_CACHE_HOME HOME="$H" \
    XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 ANDROMEDA_SOCKET="$R/andromeda-wayland-0.sock" \
    ANDROMEDA_DEV_TOKENS="$ROOT/protocol/design-tokens.toml" NOCTALIA="$NOCTALIA" LOG="$R/noctalia.log" \
    PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR" PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse/native" \
    setsid dbus-run-session -- bash -c '"$NOCTALIA" > "$LOG" 2>&1 & wait' &
  local shell=$!
  for _ in $(seq 60); do grep -q "mixer-api ready\|first frame" "$R/noctalia.log" 2>/dev/null && break; sleep 0.5; done
  sleep 3

  msg() { HOME="$H" XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 "$NOCTALIA" msg "$@" > /dev/null 2>&1; }
  # Moving content under the glass: a terminal that scrolls without pause.
  busy() { XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 foot -e sh -c 'while :; do date +%N; done' > /dev/null 2>&1 & echo $!; }

  intel_gpu_top -J -s 500 -o "$dir/gpu.json" > /dev/null 2>&1 &
  local gpu_log=$!

  : > "$dir/scenarios.txt"
  scenario() {
    local name=$1; shift
    local t0 e0 c0; t0=$(mono_ns); e0=$(energy); c0=$(cpu_ticks "$comp")
    "$@"
    local t1 e1 c1; t1=$(mono_ns); e1=$(energy); c1=$(cpu_ticks "$comp")
    local secs; secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN {printf "%.2f", (b - a) / 1e9}')
    local watts; watts=$(awk -v a="$e0" -v b="$e1" -v s="$secs" 'BEGIN {printf "%.2f", (b - a) / 1e6 / s}')
    local cpu; cpu=$(awk -v a="$c0" -v b="$c1" -v s="$secs" -v hz="$(getconf CLK_TCK)" 'BEGIN {printf "%.1f", (b - a) / hz / s * 100}')
    echo "$name $t0 $t1 $secs $watts $cpu $(rss_kb "$comp")" >> "$dir/scenarios.txt"
  }

  s_idle() { sleep "$SECONDS_PER"; }
  s_cc_over_motion() {
    local p; p=$(busy); sleep 1; msg panel-open control-center quick; sleep "$SECONDS_PER"; msg panel-close; kill "$p" 2>/dev/null
  }
  s_open_close() {
    for _ in $(seq 20); do msg panel-open control-center quick; sleep 0.25; msg panel-close; sleep 0.25; done
    for _ in $(seq 10); do msg panel-open launcher; sleep 0.25; msg panel-close; sleep 0.25; done
  }
  s_launcher_typing() {
    msg panel-open launcher; sleep 0.8
    for _ in $(seq 6); do XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 wtype "s"; sleep 0.4
      XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 wtype -k BackSpace; sleep 0.4; done
    msg panel-close
  }
  s_worst() {
    local p1 p2; p1=$(busy); p2=$(busy); sleep 1; msg panel-open launcher; sleep 0.8
    XDG_RUNTIME_DIR="$R" WAYLAND_DISPLAY=wayland-0 wtype "a"; sleep "$SECONDS_PER"; msg panel-close
    kill "$p1" "$p2" 2>/dev/null
  }

  scenario idle s_idle
  scenario cc_over_motion s_cc_over_motion
  scenario open_close s_open_close
  scenario launcher_typing s_launcher_typing
  scenario worst s_worst
  sleep 1

  [[ -n $gpu_log ]] && kill "$gpu_log" 2>/dev/null
  # The shell runs in its own session: stop the whole group (shell, bus, gvfs) so the runtime dir unmounts.
  kill -- "-$shell" 2>/dev/null; sleep 1; kill -9 -- "-$shell" 2>/dev/null
  kill "$comp" 2>/dev/null; wait "$comp" 2>/dev/null
  cp "$R/andromeda.log" "$R/noctalia.log" "$dir/" 2>/dev/null
  grep -q "Segmentation\|core dumped" "$dir/noctalia.log" 2>/dev/null && echo "WARNING: shell crashed in mode $mode" >&2
  rm -rf "$R" 2>/dev/null || true
}

for mode in "${MODES[@]}"; do
  echo "== $mode" >&2
  run_mode "$mode" || exit 1
done

# Report: per scenario and mode. Budget: 25 % of a 60 Hz frame on the target GPU, scaled to this GPU.
budget=$(awk -v s="$BUDGET_SCALE" 'BEGIN {printf "%.3f", 16.667 * 0.25 / s}')
printf '%-16s %-8s %7s %8s %8s %8s %7s %6s %8s\n' scenario mode frames p50ms p99ms maxms watts cpu% rssMB
for name in idle cc_over_motion open_close launcher_typing worst; do
  for mode in "${MODES[@]}"; do
    line=$(awk -v n="$name" '$1 == n' "$OUT/$mode/scenarios.txt")
    read -r _ t0 t1 _ watts cpu rss <<< "$line"
    read -r frames p50 p99 max <<< "$(frame_stats "$OUT/$mode/frames.log" "$t0" "$t1")"
    printf '%-16s %-8s %7s %8s %8s %8s %7s %6s %8.1f\n' "$name" "$mode" "$frames" "$p50" "$p99" "$max" "$watts" "$cpu" \
      "$(awk -v k="$rss" 'BEGIN {print k / 1024}')"
  done
done
echo "budget: p99 <= ${budget} ms (25 % of 16.7 ms, BUDGET_SCALE=$BUDGET_SCALE); idle frames must be 0"
[[ -s $OUT/${MODES[0]}/gpu.json ]] || echo "gpu busy: n/a (grant once: sudo setcap cap_perfmon+ep /usr/bin/intel_gpu_top)"
