#!/usr/bin/env bash
# A suspend or power-off that logind already runs never turns into a shell shutdown: logind lists such an action in
# ScheduledShutdown (with PreparingForSleep/PreparingForShutdown set), and cancelling it left the machine awake with
# NetworkManager asleep, then powered off after the 60 s session-end countdown. A real schedule due within a minute
# still hands over to the session end (it cancels logind's schedule). Writes calls.txt to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/power-scheduled}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
BEFORE_NOCTALIA='
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
' with_noctalia '
  schedule() {
    dbus-send --bus="$DBUS_SYSTEM_BUS_ADDRESS" --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 \
      org.andromeda.test.MockLogind.MockSchedule string:"$1" uint64:"$2" string:"$3" > /dev/null
  }
  now_usec() { echo $(( $(date +%s%N) / 1000 )); }
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock logind did not start: $(cat "$OUT/mock.log")"; exit 1; }

  for running in "suspend sleep" "poweroff shutdown"; do
    set -- $running
    schedule "$1" "$(( $(now_usec) + 15000000 ))" "$2"
    sleep 2
    if grep -q CancelScheduledShutdown "$OUT/calls.txt"; then
      echo "FAIL: the shell cancelled the running $1: $(tr "\n" "," < "$OUT/calls.txt")"
      exit 1
    fi
  done

  schedule poweroff "$(( $(now_usec) + 30000000 ))" ""
  for _ in $(seq 50); do grep -q "CancelScheduledShutdown poweroff" "$OUT/calls.txt" && exit 0; sleep 0.1; done
  echo "FAIL: a power-off due in 30 s never handed over to the session end: $(tr "\n" "," < "$OUT/calls.txt")"
  exit 1
'
echo "PASS: $(tr '\n' ',' < "$OUT/calls.txt"); artifacts: $OUT"
