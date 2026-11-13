#!/usr/bin/env bash
# A frame that blocks the main thread 250 ms or more writes a stall report only with developer diagnostics on: the
# report holds the samples and the main thread's user stack, taken on the watch thread. Off, the frame is only logged.
set -euo pipefail
readonly STALLS="$ANDROMEDA_RUNTIME_DIR/xdg-state/andromeda/stalls"

await_frame_log() {
  for _ in $(seq 40); do
    [[ $(grep -c 'frame took' "$ANDROMEDA_LOG") -ge $1 ]] && return 0
    sleep 0.1
  done
  echo "no slow-frame line number $1"
  return 1
}

"$ANDROMEDA" stall-frame 400 > /dev/null
await_frame_log 1
sleep 0.3 # real time: a report, if any, is written on the watch thread right after the frame ends
if compgen -G "$STALLS/*.txt" > /dev/null; then
  echo "a stall report was written with developer diagnostics off"
  exit 1
fi

sed -i 's/^\[general\]$/[general]\ndeveloper_diagnostics = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" stall-frame 400 > /dev/null
await_frame_log 2
for _ in $(seq 40); do
  compgen -G "$STALLS/*.txt" > /dev/null && break
  sleep 0.1
done
report=$(compgen -G "$STALLS/*.txt" | head -n 1) || { echo "no stall report with developer diagnostics on"; exit 1; }
grep -q '^component: andromeda$' "$report" || { echo "report lacks the component: $(head -n 3 "$report")"; exit 1; }
duration=$(sed -n 's/^duration_ms: //p' "$report")
(( duration >= 400 && duration < 1000 )) || { echo "report duration $duration ms, want 400-999 (the capture must not lengthen the stall)"; exit 1; }
grep -q 'wchan=hrtimer_nanosleep\|wchan=do_nanosleep' "$report" || { echo "report lacks the sleep sample: $(grep samples "$report")"; exit 1; }
sed -n '/^user_stack:/,/^kernel_stacks:/p' "$report" | grep -q ' usleep$' \
  || { echo "user stack lacks the stalled frame: $(sed -n '/^user_stack:/,/^kernel_stacks:/p' "$report" | head -n 12)"; exit 1; }
echo "a 400 ms frame writes one report with samples and the main thread's stack, only with developer diagnostics on"
