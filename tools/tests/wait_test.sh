#!/usr/bin/env bash
# Checks tools/wait.sh: log match, fail pattern, timeout, pid exit and file appearance.
set -uo pipefail
w=$(cd "$(dirname "$0")/.." && pwd)/wait.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# check <name> <want rc> <want output ERE> <max secs> <cmd...>
check() {
  local name=$1 want=$2 re=$3 max=$4 got rc
  shift 4
  SECONDS=0
  got=$("$@")
  rc=$?
  if [ "$rc" -eq "$want" ] && [[ $got =~ $re ]] && [ "$SECONDS" -le "$max" ]; then
    echo "pass $name: $got"
  else
    echo "FAIL $name: rc $rc (want $want) after ${SECONDS}s (max $max): $got"
    fail=1
  fi
}

log=$tmp/a.log
echo "starting" > "$log"
setsid -f bash -c 'sleep 2; echo "server READY on :8080" >> "$0"' "$log"
check match 0 '^wait: matched "server READY on :8080" after [0-9]+s$' 5 "$w" --log "$log" --match 'READY' --timeout 20

log=$tmp/b.log
: > "$log"
setsid -f bash -c 'sleep 1; echo "boot"; echo "fatal: socket in use"' > "$log"
check fail-pattern 1 '^wait: FAILED matched "fatal: socket in use"' 5 "$w" --log "$log" --match 'READY' --fail '^fatal' --timeout 20

log=$tmp/long.log
printf '%0300d ERROR: disk full\n' 0 > "$log"
check fail-long-line 1 '^wait: FAILED matched "0{200}" after' 3 "$w" --log "$log" --match 'disk' --fail 'ERROR' --timeout 5

log=$tmp/c.log
printf 'one\nlast line here\n' > "$log"
check timeout 124 '^wait: timeout after 2s; last log line: last line here$' 4 "$w" --log "$log" --match 'READY' --timeout 2

setsid -f bash -c 'echo $$ > "$0.tmp"; mv "$0.tmp" "$0"; sleep 2' "$tmp/pid"
check file 0 "^wait: matched \"$tmp/pid ready\"" 4 "$w" --file "$tmp/pid" --timeout 10
pid=$(cat "$tmp/pid")
check pid 0 "^wait: matched \"pid $pid exited\"" 5 "$w" --pid "$pid" --timeout 10

check pid-timeout 124 '^wait: timeout after 1s; pid 1 still running$' 3 "$w" --pid 1 --timeout 1

exit "$fail"
