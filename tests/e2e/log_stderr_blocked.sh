#!/usr/bin/env bash
# Repro: the compositor's stderr is a pipe nobody reads. Once the 64 KiB pipe is full, a synchronous log write blocks
# the main thread. PASS = the compositor still answers IPC for 15 s with debug logging on.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-debug/andromeda}
RT=$(mktemp -d /tmp/logblock.XXXX)
mkfifo "$RT/err"
exec 3<>"$RT/err"            # holds the read end open and never reads
# The smallest pipe buffer (one page), so a few dozen lines fill it.
python3 -I -c "import fcntl; fcntl.fcntl(3, 1031, 4096)"
printf '[general]\nautostart = []\n' > "$RT/a.toml"
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$RT" XDG_CACHE_HOME="$RT/cache" ANDROMEDA_WLR_DEBUG=1 \
  WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1 \
  "$ANDROMEDA" -c "$RT/a.toml" > "$RT/err" 2>&1 &
pid=$!
for _ in $(seq 100); do [[ -S $RT/andromeda-wayland-0.sock ]] && break; sleep 0.05; done
ipc() { timeout 2 env XDG_RUNTIME_DIR="$RT" ANDROMEDA_SOCKET="$RT/andromeda-wayland-0.sock" "$ANDROMEDA" msg config-reload > /dev/null 2>&1; }
fail=0
for i in $(seq 30); do
  ipc || { echo "FAIL: no IPC answer at try $i (pipe full: $(timeout 1 cat /proc/$pid/wchan 2>/dev/null))"; fail=1; break; }
  sleep 0.5
done
kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null
lines=$(grep -c . "$RT/cache/andromeda/andromeda.log" 2>/dev/null); lines=${lines:-0}
echo "log file lines: $lines"
(( lines > 0 )) || { echo "FAIL: the log file got no lines while stderr was blocked"; fail=1; }
rm -rf "$RT"
(( fail == 0 )) && echo "PASS: IPC answered for 15 s with stderr blocked"
exit $fail
