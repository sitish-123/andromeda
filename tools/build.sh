#!/usr/bin/env bash
# Usage: tools/build.sh <command...>   e.g. tools/build.sh just build, tools/build.sh just check
# Runs one heavy build or test at a time machine-wide (flock on /tmp/desktop-build.lock), pinned to 6 cores (ninja
# sizes its job count from CPU affinity), in its own memory-capped systemd scope. Under memory pressure systemd-oomd
# then kills this build, not the terminal hosting the session and its agents, which it did twice when three
# worktrees compiled at once. --close keeps the lock off child processes, so an orphaned test helper cannot hold it.
set -euo pipefail
exec flock --close /tmp/desktop-build.lock \
  systemd-run --user --scope --quiet --collect -p MemoryHigh=12G -p MemoryMax=16G -p MemorySwapMax=4G \
  taskset -c 0-5 nice -n 10 "$@"
