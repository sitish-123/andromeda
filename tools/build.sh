#!/usr/bin/env bash
# Usage: tools/build.sh <command...>   e.g. tools/build.sh just build, tools/build.sh ninja -C shell/build-debug
# Runs heavy builds and tests machine-wide in two slots (P-cores 0-5, E-cores 6-11; 12-15 stay free for the desktop and
# the sessions), each in its own memory-capped systemd scope, so systemd-oomd kills a build under memory pressure, not
# the terminal hosting the sessions. ninja sizes its job count from the slot's CPU affinity.
# Sessions share build dirs: a `-C <dir>` command also holds a lock on that dir, so two ninjas never run in one dir, and
# a caller whose identical command was run and passed after it asked exits 0 at once (that run covered its request).
# --close keeps the locks off child processes, so an orphaned test helper cannot hold them.
# A nested call (tools/f7.sh inside this script) runs at once: the outer call holds the locks.
set -uo pipefail
if [ -n "${DESKTOP_BUILD_LOCKED:-}" ]; then
  exec "$@"
fi
# build.ninja calls bare cc/c++, so the ccache wrappers in PATH cache every build dir with no meson re-setup.
[ -d /usr/lib/ccache/bin ] && export PATH=/usr/lib/ccache/bin:$PATH
state=/tmp/desktop-build-state
mkdir -p "$state"
asked=$(date +%s%N)

dir=
args=("$@")
for ((i = 0; i < ${#args[@]} - 1; i++)); do
  [ "${args[$i]}" = -C ] && dir=$(realpath -m "${args[$((i + 1))]}")
done
key=$(printf '%s\0' "$PWD" "$@" | md5sum | cut -c1-16)

# Inner stage: holds the dir lock (if any), skips a covered request, then takes the first free slot.
if [ -n "${DESKTOP_BUILD_DIR_HELD:-}" ]; then
  asked=$DESKTOP_BUILD_DIR_HELD
  if [ -f "$state/$key" ]; then
    read -r started rc < "$state/$key"
    if [ "$rc" = 0 ] && [ "$started" -gt "$asked" ]; then
      echo "build: covered by a passed run of the same command that started after this request"
      exit 0
    fi
  fi
  cores=(0-5 6-11)
  last=$SECONDS
  while :; do
    for s in 0 1; do
      started=$(date +%s%N)
      flock -n -E 75 --close "/tmp/desktop-build.$s.lock" \
        systemd-run --user --scope --quiet --collect -p MemoryHigh=9G -p MemoryMax=12G -p MemorySwapMax=4G \
        --setenv=DESKTOP_BUILD_LOCKED=1 taskset -c "${cores[$s]}" nice -n 10 "$@"
      rc=$?
      if [ "$rc" -ne 75 ]; then
        echo "$started $rc" > "$state/$key"
        exit "$rc"
      fi
    done
    # A line per minute keeps stall watchdogs from stopping the wait.
    if ((SECONDS - last >= 60)); then
      echo "build: waiting for a build slot ($(date +%T))"
      last=$SECONDS
    fi
    flock -w 5 /tmp/desktop-build.0.lock true 2> /dev/null
  done
fi

if [ -z "$dir" ]; then
  DESKTOP_BUILD_DIR_HELD=$asked exec "$0" "$@"
fi
dirlock=/tmp/desktop-build-dir.$(printf '%s' "$dir" | md5sum | cut -c1-16).lock
while :; do
  # 75 = dir lock not free within 60 s.
  flock -w 60 -E 75 --close "$dirlock" env DESKTOP_BUILD_DIR_HELD="$asked" "$0" "$@"
  rc=$?
  [ "$rc" -eq 75 ] || exit "$rc"
  echo "build: waiting for $dir ($(date +%T))"
done
