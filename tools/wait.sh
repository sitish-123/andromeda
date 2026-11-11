#!/usr/bin/env bash
# Usage: tools/wait.sh --log <file> --match <ERE> [--fail <ERE>] --timeout <secs>
#        tools/wait.sh --pid <pid> --timeout <secs>
#        tools/wait.sh --file <path> --timeout <secs>          (exists and non-empty)
#        tools/wait.sh --dbus <name> [--system] --timeout <secs>  (name has an owner)
# Blocks on the event, not on a sleep loop. Exit 0 matched, 1 fail pattern matched, 124 timeout, 2 usage.
set -uo pipefail
usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

mode= target= match= fail= timeout= bus=--session
while [ $# -gt 0 ]; do
  case $1 in
    --log | --pid | --file | --dbus) [ -z "$mode" ] || usage; mode=${1#--}; target=${2-}; shift 2 || usage ;;
    --match) match=${2-}; shift 2 || usage ;;
    --fail) fail=${2-}; shift 2 || usage ;;
    --timeout) timeout=${2-}; shift 2 || usage ;;
    --system) bus=--system; shift ;;
    *) usage ;;
  esac
done
[ -n "$mode" ] && [ -n "$target" ] && [[ $timeout =~ ^[0-9]+$ ]] && [ "$timeout" -gt 0 ] || usage
[ "$mode" != log ] || [ -n "$match" ] || usage
[ "$mode" != pid ] || [[ $target =~ ^[0-9]+$ ]] || usage
[ "$timeout" -le 600 ] || { echo "wait: timeout capped at 600s"; timeout=600; }

out=$(mktemp)
waiter=
timer=
cleanup() { kill -- ${waiter:+-"$waiter"} ${timer:+-"$timer"} 2> /dev/null; rm -f "$out"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

# Each helper runs in its own process group, so one kill also stops its tail, grep and gdbus children.
case $mode in
  log)
    pat=$match
    [ -z "$fail" ] || pat="($match)|($fail)"
    # Process substitution: the shell exits with grep and does not wait for tail.
    setsid bash -c 'grep -m1 -E -- "$1" < <(exec tail -n +1 -F -- "$2" 2> /dev/null)' _ "$pat" "$target" > "$out" &
    ;;
  pid) setsid tail --pid="$target" -f /dev/null & ;;
  file) setsid bash -c 'head -c1 < <(exec tail -c +1 -F -- "$1" 2> /dev/null) > /dev/null' _ "$target" & ;;
  dbus) setsid gdbus wait "$bus" -- "$target" > /dev/null 2>&1 & ;;
esac
waiter=$!

# The timer prints a line each 60 s for stall watchdogs, then stops the waiter at the timeout.
setsid bash -c 'n=0; while [ "$n" -lt "$1" ]; do s=$(( $1 - n < 60 ? $1 - n : 60 )); sleep "$s"; n=$((n + s));
  [ "$n" -ge "$1" ] || echo "wait: still waiting (${n}s)"; done; kill -- -"$2" 2> /dev/null' _ "$timeout" "$waiter" &
timer=$!

SECONDS=0
wait "$waiter"
rc=$?

if [ "$mode" = log ]; then [ -s "$out" ]; else [ "$rc" -eq 0 ]; fi || {
  # 143: the timer's SIGTERM stopped the waiter.
  if [ "$rc" -ne 143 ]; then
    echo "wait: error: the $mode waiter exited with $rc after ${SECONDS}s"
    exit 2
  fi
  case $mode in
    log) echo "wait: timeout after ${timeout}s; last log line: $(tail -n 1 -- "$target" 2> /dev/null | cut -c1-200)" ;;
    pid) echo "wait: timeout after ${timeout}s; pid $target still running" ;;
    file) echo "wait: timeout after ${timeout}s; $target missing or empty" ;;
    dbus) echo "wait: timeout after ${timeout}s; $target has no owner" ;;
  esac
  exit 124
}

case $mode in
  log) line=$(head -n 1 "$out") ;;
  pid) line="pid $target exited" ;;
  file) line="$target ready" ;;
  dbus) line="$target owned" ;;
esac
if [ "$mode" = log ] && [ -n "$fail" ] && grep -qE -- "$fail" <<< "$line"; then
  echo "wait: FAILED matched \"${line:0:200}\" after ${SECONDS}s"
  exit 1
fi
echo "wait: matched \"${line:0:200}\" after ${SECONDS}s"
