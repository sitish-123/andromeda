#!/usr/bin/env bash
# Usage: tools/build.sh <command...>   e.g. tools/build.sh just build, tools/build.sh ninja -C shell/build-debug
# Runs one heavy build or test at a time machine-wide (flock on /tmp/desktop-build.lock), pinned to 6 cores (ninja
# sizes its job count from CPU affinity), in its own memory-capped systemd scope. Under memory pressure systemd-oomd
# then kills this build, not the terminal hosting the session and its agents, which it did twice when three
# worktrees compiled at once. --close keeps the lock off child processes, so an orphaned test helper cannot hold it.
# A nested call (tools/f7.sh inside this script) runs at once: the outer call holds the lock.
set -uo pipefail
if [ -n "${DESKTOP_BUILD_LOCKED:-}" ]; then
  exec "$@"
fi
export DESKTOP_BUILD_LOCKED=1
# build.ninja calls bare cc/c++, so the ccache wrappers in PATH cache every build dir with no meson re-setup.
[ -d /usr/lib/ccache/bin ] && export PATH=/usr/lib/ccache/bin:$PATH
lock=/tmp/desktop-build.lock
while :; do
  # 75 = lock not free within 60 s; a line per minute keeps stall watchdogs from stopping the wait.
  flock -w 60 -E 75 --close "$lock" \
    systemd-run --user --scope --quiet --collect -p MemoryHigh=12G -p MemoryMax=16G -p MemorySwapMax=4G \
    --setenv=DESKTOP_BUILD_LOCKED=1 taskset -c 0-5 nice -n 10 "$@"
  rc=$?
  [ "$rc" -eq 75 ] || exit "$rc"
  echo "build: waiting for $lock ($(date +%T))"
done
