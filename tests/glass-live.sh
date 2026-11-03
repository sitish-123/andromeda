#!/usr/bin/env bash
# Glass soak in a real running session (the test laptop): moving content under every glass surface, with per-scenario
# GPU frame time, missed frames, compositor and shell CPU, RSS and package power. Runs inside the user's session.
# Needs: the compositor started with ANDROMEDA_FRAME_LOG=$OUT/frames.log, and foot, mpv, ffmpeg, ydotool, notify-send.
# Usage: OUT=/tmp/glass-live SECONDS_PER=12 RECORD=0 tests/glass-live.sh
#   RECORD=1 also records the screen to $OUT/run.mp4 (wf-recorder costs GPU time: use it on a second pass, not for numbers).
set -uo pipefail
OUT=${OUT:-/tmp/glass-live}
SECONDS_PER=${SECONDS_PER:-12}
RECORD=${RECORD:-0}
GLASS=${GLASS:-}
# REDUCE=1 runs with glass off (Reduce Transparency): the baseline to subtract from a glass run.
REDUCE=${REDUCE:-0}
SHELL_CONF=$HOME/.config/noctalia/config.toml
SETTINGS=${SETTINGS:-$HOME/.config/andromeda/settings.toml}
FRAMES=${FRAMES:-$OUT/frames.log}
RAPL=/sys/class/powercap/intel-rapl:0/energy_uj
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
mkdir -p "$OUT"
[[ -s $FRAMES ]] || { echo "no frame log at $FRAMES: start the compositor with ANDROMEDA_FRAME_LOG=$FRAMES" >&2; exit 2; }
# Typing goes through ydotool (a kernel input device, like a real keyboard): wtype uploads a new keymap per character,
# and the shell's keymap parse then dominates its CPU profile.
export YDOTOOL_SOCKET=${YDOTOOL_SOCKET:-$XDG_RUNTIME_DIR/.ydotool_socket}
type_text() { ydotool type --key-delay "$1" "$2" > /dev/null 2>&1; }
backspaces() { for _ in $(seq "$1"); do ydotool key 14:1 14:0 > /dev/null 2>&1; done; }
for t in foot mpv ffmpeg ydotool notify-send; do command -v "$t" > /dev/null || { echo "missing $t" >&2; exit 2; }; done

comp=$(pgrep -xn -u "$(id -u)" andromeda); shellpid=$(pgrep -xn -u "$(id -u)" noctalia)
glass0=$(sed -n 's/^glass *= *//p' "$SETTINGS" | head -1); scheme0=$(sed -n "s/^scheme *= *'\?\([a-z]*\)'\?/\1/p" "$SETTINGS" | head -1)
reduce0=$(sed -n 's/^reduce_transparency *= *\([a-z]*\).*/\1/p' "$SETTINGS" | head -1)
restore() {
  if [[ -n $reduce0 ]]; then set_reduce "$reduce0"; else sed -i '/^reduce_transparency *=/d' "$SETTINGS"; fi
  [[ -f $SHELL_CONF.glass-live ]] && mv "$SHELL_CONF.glass-live" "$SHELL_CONF"
  [[ -f $SHELL_CONF.glass-live-none ]] && rm -f "$SHELL_CONF" "$SHELL_CONF.glass-live-none"
  set_glass "${glass0:-0.5}"; set_scheme "${scheme0:-light}"
  noctalia msg panel-close > /dev/null 2>&1 || true
  kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null
}
trap restore EXIT
set_glass() { sed -i "s/^glass *=.*/glass = $1/" "$SETTINGS"; }
# A second reduce_transparency key makes the whole settings file fail to parse.
set_reduce() {
  sed -i '/^reduce_transparency = true # glass-live$/d' "$SETTINGS"
  if grep -q '^reduce_transparency *=' "$SETTINGS"; then sed -i "s/^reduce_transparency *=.*/reduce_transparency = $1/" "$SETTINGS"
  else sed -i "/^\[appearance\]/a reduce_transparency = $1" "$SETTINGS"; fi
}
set_scheme() { sed -i "s/^scheme *=.*/scheme = '$1'/" "$SETTINGS"; }
msg() { noctalia msg "$@" > /dev/null 2>&1; }
energy() { sudo -n cat "$RAPL" 2> /dev/null || echo 0; }
ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2> /dev/null || echo 0; }
rss() { awk '/^VmRSS/ {print int($2 / 1024)}' "/proc/$1/status" 2> /dev/null || echo 0; }
mono_ns() { python3 -c 'import time; print(time.monotonic_ns())'; }

# Moving content: a 1080p colour test pattern with a moving bar, and a terminal that scrolls coloured text non-stop.
[[ -s $OUT/motion.mkv ]] || ffmpeg -loglevel error -f lavfi -i "testsrc2=size=1920x1080:rate=60,format=yuv420p" -t 30 \
  -c:v libx264 -preset ultrafast "$OUT/motion.mkv"
# A tiled window takes only a column: test windows run fullscreen (video cropped to fill) so every sheet, banner and
# the center island sit over moving content. Fullscreen hides the side bar islands and the dock.
edges() { sleep 1.5; }
video() { mpv --really-quiet --loop=inf --no-audio --fs --panscan=1 --title=glass-video "$OUT/motion.mkv" > /dev/null 2>&1 & echo $!; }
scroller() {
  foot --fullscreen --title=glass-scroll -o colors-dark.background=1e1e2e -o colors-light.background=fdf6e3 sh -c \
    'W=$(printf "The quick brown fox jumps over the lazy dog | %.0s" 1 2 3 4 5 6); i=0; while :; do i=$((i+1)); printf "\033[3%dm%06d %s %s\033[0m\n" $((i%7+1)) $i "$(date +%N)" "$W"; done' \
    > /dev/null 2>&1 & echo $!
}

: > "$OUT/scenarios.txt"
scenario() {
  local name=$1; shift
  local t0 e0 c0 s0; t0=$(mono_ns); e0=$(energy); c0=$(ticks "$comp"); s0=$(ticks "$shellpid")
  "$@"
  local t1 e1 c1 s1; t1=$(mono_ns); e1=$(energy); c1=$(ticks "$comp"); s1=$(ticks "$shellpid")
  echo "$name $t0 $t1 $e0 $e1 $c0 $c1 $s0 $s1 $(rss "$comp") $(rss "$shellpid")" >> "$OUT/scenarios.txt"
  grim -s 0.5 "$OUT/$name.png" 2> /dev/null || true
}

# The bar islands and the dock over motion: they stop reserving space, and a maximized video runs under them.
s_bar_dock_over_video() {
  if [[ -f $SHELL_CONF ]]; then cp "$SHELL_CONF" "$SHELL_CONF.glass-live"; else : > "$SHELL_CONF.glass-live-none"; fi
  printf '\n[bar.default]\nreserve_space = false\n\n[dock]\nreserve_space = false\n' >> "$SHELL_CONF"
  sleep 1
  mpv --really-quiet --loop=inf --no-audio --panscan=1 --title=glass-video "$OUT/motion.mkv" > /dev/null 2>&1 &
  local v=$!; sleep 1.2; andromeda msg window-toggle-maximize-to-edges > /dev/null 2>&1
  sleep "$SECONDS_PER"; kill "$v"
  if [[ -f $SHELL_CONF.glass-live ]]; then mv "$SHELL_CONF.glass-live" "$SHELL_CONF"; else rm -f "$SHELL_CONF" "$SHELL_CONF.glass-live-none"; fi
  sleep 1
}
s_idle() { sleep "$SECONDS_PER"; }
s_video_full() { local v; v=$(video); edges; sleep "$SECONDS_PER"; kill "$v"; }
s_cc_over_video() {
  local v; v=$(video); edges
  local end=$((SECONDS + SECONDS_PER))
  while ((SECONDS < end)); do msg panel-open control-center; sleep 2; msg panel-close control-center; sleep 0.7; done
  kill "$v"
}
s_launcher_typing_over_scroll() {
  local f; f=$(scroller); edges
  msg panel-open launcher; sleep 0.5
  local end=$((SECONDS + SECONDS_PER))
  while ((SECONDS < end)); do type_text 60 "firefox settings terminal"; backspaces 25; done
  msg panel-close launcher; kill "$f"
}
s_notifications_over_video() {
  local v; v=$(video); edges
  for i in $(seq "$SECONDS_PER"); do notify-send -a glass-live -h "string:x-canonical-private-synchronous:glass$i" "Glass test $i" "Banner over moving video"; sleep 1; done
  kill "$v"
}
s_slider_sweep_over_video() {
  local v; v=$(video); edges; msg panel-open control-center
  local end=$((SECONDS + SECONDS_PER))
  while ((SECONDS < end)); do for g in 0.0 0.25 0.5 0.75 1.0 0.5; do set_glass "$g"; sleep 0.5; done; done
  msg panel-close control-center; kill "$v"
}
s_scheme_flip_over_scroll() {
  local f; f=$(scroller); edges; msg panel-open control-center
  local end=$((SECONDS + SECONDS_PER))
  while ((SECONDS < end)); do set_scheme dark; sleep 1.5; set_scheme light; sleep 1.5; done
  msg panel-close control-center; kill "$f"
}
s_worst() {
  local v; v=$(video); edges
  msg panel-open launcher
  local end=$((SECONDS + SECONDS_PER))
  while ((SECONDS < end)); do type_text 40 "glass"; backspaces 5
    notify-send -a glass-live -h string:x-canonical-private-synchronous:worst "Worst case" "$SECONDS"; done
  msg panel-close launcher; kill "$v"
}

[[ -n $GLASS ]] && { set_glass "$GLASS"; sleep 1; }
set_reduce "$([[ $REDUCE == 1 ]] && echo true || echo false)"; sleep 1
rec=
if [[ $RECORD == 1 ]]; then wf-recorder -f "$OUT/run.mp4" > "$OUT/wf-recorder.log" 2>&1 & rec=$!; sleep 1; fi
for s in idle bar_dock_over_video video_full cc_over_video launcher_typing_over_scroll notifications_over_video slider_sweep_over_video \
  scheme_flip_over_scroll worst; do
  scenario "$s" "s_$s"
  sleep 1
done
[[ -n $rec ]] && { kill -INT "$rec"; wait "$rec" 2> /dev/null; }

# Report. frames.log: start_ns output cpu_ns gpu_ns. A missed frame = more than 1.5 refresh intervals since the last one.
hz_ms=16.68
printf '%-30s %6s %7s %7s %7s %6s %6s %7s %6s %6s %6s %7s %6s %7s %5s\n' scenario frames p50ms p99ms maxms missed watts comp% shell% compMB shellMB blurKpx lens bdKpx fp16 | tee "$OUT/report.txt"
hz=$(getconf CLK_TCK)
while read -r name t0 t1 e0 e1 c0 c1 s0 s1 crss srss; do
  awk -v from="$t0" -v to="$t1" '$1 >= from && $1 < to {print $1, $4}' "$FRAMES" | sort -n > "$OUT/.f"
  read -r n p50 p99 mx missed < <(awk -v hz="$hz_ms" '
    {t[NR] = $1; g[NR] = $2} END {
      if (NR == 0) {print "0 0 0 0 0"; exit}
      m = 0; for (i = 2; i <= NR; i++) if ((t[i] - t[i-1]) / 1e6 > 1.5 * hz) m++
      n = asort(g); i50 = int(n * 0.5); i99 = int(n * 0.99); if (i50 < 1) i50 = 1; if (i99 < 1) i99 = 1
      printf "%d %.2f %.2f %.2f %d\n", NR, g[i50] / 1e6, g[i99] / 1e6, g[n] / 1e6, m}' "$OUT/.f")
  # Effect work per frame (frame log columns 5-11, compositors with fx_frame_stats): mean blur area, lens copies,
  # backdrop rebuild area and FP16 passes.
  read -r bkpx lensn bdkpx fp16 < <(awk -v from="$t0" -v to="$t1" '$1 >= from && $1 < to && NF >= 11 {n++; b += $6; l += $7; d += $10; f += $11}
    END {if (n == 0) print "- - - -"; else printf "%.0f %.2f %.0f %.2f\n", b / n / 1000, l / n, d / n / 1000, f / n}' "$FRAMES")
  secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN {print (b - a) / 1e9}')
  watts=$(awk -v a="$e0" -v b="$e1" -v s="$secs" 'BEGIN {print (b > a && s > 0) ? sprintf("%.2f", (b - a) / 1e6 / s) : "n/a"}')
  cpu() { awk -v a="$1" -v b="$2" -v s="$secs" -v hz="$hz" 'BEGIN {printf "%.1f", (b - a) / hz / s * 100}'; }
  printf '%-30s %6d %7s %7s %7s %6d %6s %7s %6s %6s %6s %7s %6s %7s %5s\n' "$name" "$n" "$p50" "$p99" "$mx" "$missed" "$watts" \
    "$(cpu "$c0" "$c1")" "$(cpu "$s0" "$s1")" "$crss" "$srss" "$bkpx" "$lensn" "$bdkpx" "$fp16" | tee -a "$OUT/report.txt"
done < "$OUT/scenarios.txt"
echo "budget: p99 GPU <= 4.17 ms (25 % of a 60 Hz frame); missed frames should be 0 outside idle" | tee -a "$OUT/report.txt"
