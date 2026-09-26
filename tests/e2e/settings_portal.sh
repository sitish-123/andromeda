#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.Settings: ReadAll and Read report org.freedesktop.appearance
# color-scheme (1 dark, 2 light), switching the theme mode emits SettingChanged, no gsettings or dconf process is
# started (a recording stub shadows both on PATH), and the shell's memory stays flat. Writes result.txt to $OUT
# (default ./artifacts/settings-portal).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-portal}
source "$(dirname "$0")/lib.sh"
boot_headless 1

mkdir -p "$RUNTIME/stub"
for tool in gsettings dconf; do
  printf '#!/bin/sh\necho "%s $*" >> "%s/spawned.txt"\n' "$tool" "$OUT" > "$RUNTIME/stub/$tool"
  chmod +x "$RUNTIME/stub/$tool"
done
: > "$OUT/spawned.txt"
export PATH="$RUNTIME/stub:$PATH"

with_noctalia '
  impl() {
    busctl --user call org.freedesktop.impl.portal.desktop.noctalia /org/freedesktop/portal/desktop \
      org.freedesktop.impl.portal.Settings "$@"
  }
  rss() { awk "/VmRSS/ {print \$2}" /proc/$(pgrep -f "^$NOCTALIA\$" | head -1)/status; }
  scheme() { impl Read ss org.freedesktop.appearance color-scheme | awk "{print \$NF}"; }
  wait_scheme() {
    for _ in $(seq 50); do [[ $(scheme) == "$1" ]] && return 0; sleep 0.1; done
    echo "FAIL: color-scheme is $(scheme), wanted $1"; exit 1
  }
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  wait_scheme 1
  impl ReadAll as 1 org.freedesktop.appearance > "$OUT/readall.txt"
  grep -q "color-scheme" "$OUT/readall.txt" || { echo "FAIL: ReadAll lacks color-scheme"; exit 1; }
  dbus-monitor --session "type=signal,interface=org.freedesktop.impl.portal.Settings,member=SettingChanged" \
    > "$OUT/signals.txt" 2>&1 &
  monitor=$!
  sleep 0.5 # real time: the monitor subscribes
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  wait_scheme 2
  sleep 0.5 # real time: the signal reaches the monitor
  kill "$monitor"
  grep -q "uint32 2" "$OUT/signals.txt" || { echo "FAIL: no SettingChanged to light"; exit 1; }
  first=$(rss); sleep 3; second=$(rss) # real time: memory must not grow at idle
  echo "rss_kb first=$first second=$second" | tee "$OUT/result.txt"
  (( second - first < 20000 && second < 400000 )) || { echo "FAIL: shell memory grew"; exit 1; }
'
[[ ! -s $OUT/spawned.txt ]] || { echo "FAIL: spawned $(cat "$OUT/spawned.txt")"; exit 1; }
echo "PASS; artifacts: $OUT"
