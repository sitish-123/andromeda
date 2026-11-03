#!/usr/bin/env bash
# Usage: tools/f7.sh [ninja targets...]   (no targets = everything)
# One shared shell/build-f7 for every session. Each caller queues its targets, then waits for the lock. The caller that
# gets the lock builds the targets of every queued caller in one ninja run; a caller whose request is older than the
# start of a passed build that covers its targets exits 0 at once, because that build already compiled its edits.
# Prints a line every 60 s while it waits, so stall watchdogs do not stop it. Exit = ninja's exit code.
set -uo pipefail
cd "$(dirname "$0")/.."
state=${F7_STATE:-/tmp/f7-build}
mkdir -p "$state"
# The bash-guard hook reads this file and blocks any other ninja in this dir.
echo 'tools/f7.sh [targets] (from the repo root)' >shell/build-f7/.shared-build
lock=${F7_LOCK:-/tmp/build-f7.lock}
targets=${*:-all}
requested=$(date +%s.%N)
echo "$targets" >>"$state/queue"

# covered <start> <built targets>: the passed build started after this request and built all of its targets.
covered() {
  [ -f "$state/last-ok" ] || return 1
  read -r start built <"$state/last-ok"
  awk -v s="$start" -v r="$requested" 'BEGIN { exit !(s > r) }' || return 1
  [ "$built" = all ] && return 0
  for t in $targets; do case " $built " in *" $t "*) ;; *) return 1 ;; esac; done
}

exec 9>"$lock"
until flock -w 60 9; do
  echo "f7: waiting for the build lock ($(date +%T))"
done
if covered; then
  echo "f7: a build that started after this request passed and covers [$targets]; nothing to do"
  exit 0
fi
# Take every queued request; "all" wins over target lists.
mv "$state/queue" "$state/queue.taken" 2>/dev/null || true
if grep -qx all "$state/queue.taken" 2>/dev/null; then
  union=all
else
  union=$(tr ' ' '\n' <"$state/queue.taken" 2>/dev/null | sort -u | xargs)
  union=${union:-$targets}
fi
rm -f "$state/queue.taken"
# An object with an empty deps record (a cut .ninja_deps) never rebuilds on a header change, and a stale struct
# layout crashes the shell at start. Delete such objects so this build compiles them again with their deps.
stale=$(ninja -C shell/build-f7 -t deps 2>/dev/null | awk '/#deps 0,/ { sub(/:$/, "", $1); print $1 }')
for obj in $stale; do rm -f "shell/build-f7/$obj"; done
[ -z "$stale" ] || echo "f7: rebuilding $(wc -w <<<"$stale") object(s) with no deps record"
start=$(date +%s.%N)
echo "f7: building [$union] (log $state/last.log)"
if [ "$union" = all ]; then
  ninja -C shell/build-f7 2>&1 | tee "$state/last.log"
else
  # shellcheck disable=SC2086
  ninja -C shell/build-f7 $union 2>&1 | tee "$state/last.log"
fi
rc=${PIPESTATUS[0]}
[ "$rc" -ne 0 ] || echo "$start $union" >"$state/last-ok"
exit "$rc"
