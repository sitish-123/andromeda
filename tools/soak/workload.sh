#!/bin/bash
# Soak workload, runs on the laptop in the user session. One cycle every <period> s: a work phase, then idle.
# Usage: workload.sh <out-dir> <minutes> [period-s]
set -u
out=${1:?out dir}
minutes=${2:?minutes}
period=${3:-300}
log=$out/workload.log
pw=/run/user/1000/laptop-pw
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export ANDROMEDA_SOCKET=/run/user/1000/andromeda-wayland-0.sock YDOTOOL_SOCKET=/tmp/.ydotool_socket
end=$(($(date +%s) + minutes * 60))
lock_ok=1

say() { printf '%s\t%s\n' "$(date +%s)" "$*" >>"$log"; }
act() { local r; r=$("$@" 2>&1 | tr '\n\t' '  '); say "act" "$*" "=> ${r:0:80}"; }
locked() { noctalia msg status | grep -q '"locked": true'; }
wait_lock() { for _ in $(seq 40); do [[ $(locked && echo 1 || echo 0) == "$1" ]] && return 0; sleep 0.25; done; return 1; }
unlock() {
  tr -d '\n' <"$pw" | ydotool type -f -
  ydotool key 28:1 28:0
  wait_lock 0
}

cycle() {
  say phase work
  if locked; then
    say event "found locked at cycle start"
    unlock && say event "unlocked by typing" || { say event "typing did not unlock"; return; }
  fi
  local pids=() p
  for i in 1 2 3; do
    foot -a soak-foot >/dev/null 2>&1 &
    pids+=($!)
    sleep 1
  done
  say act "foot x3 pids ${pids[*]}"
  sleep 3
  act andromeda msg workspace-next; sleep 2
  act andromeda msg workspace-previous; sleep 2
  act andromeda msg overview-toggle; sleep 3
  act andromeda msg overview-toggle; sleep 2
  act noctalia msg panel-open launcher; sleep 3
  act noctalia msg panel-close; sleep 2
  act noctalia msg panel-open control-center; sleep 3
  act noctalia msg panel-close; sleep 2
  for i in 1 2 3; do act notify-send -a soak "Soak $i" "cycle $(date +%T)"; sleep 1; done
  sleep 5
  act noctalia msg notification-clear-active
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  say act "foot closed"
  sleep 2
  if ((lock_ok)); then
    act noctalia msg session lock
    if wait_lock 1; then
      sleep 3
      if unlock; then say act "unlock by typing ok"; else say event "typing did not unlock: lock cycles stop"; lock_ok=0; fi
    else
      say event "lock did not engage"
    fi
  fi
  say phase idle
}

say start "period ${period}s minutes $minutes"
while (($(date +%s) < end)); do
  t0=$(date +%s)
  cycle
  rest=$((period - ($(date +%s) - t0)))
  ((rest > 0)) && sleep "$rest"
done
say end
