#!/usr/bin/env bash
# core/process/process.cpp: a callback-based process::runAsync (here, a theme template's post_hook,
# hook_async default true) completes through process::AsyncProcessManager, polled by the shell's
# main loop over a pidfd, instead of a helper thread spawned per call. Configures a user template
# whose post_hook sleeps 1 s then touches a marker file, switches theme mode to trigger it, and asserts: the marker
# appears (the completion callback fired end to end), and the shell's peak thread count while the hook runs is no higher
# than before it (no thread waits on it). Writes thread-count.txt to
# $OUT (default ./artifacts/process-async-callback).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/process-async-callback}
source "$(dirname "$0")/lib.sh"
boot_headless 1

printf 'static template content, no color tokens needed\n' > "$RUNTIME/hook_input.tmpl"
{
  printf '[plugins]\nauto_update = "none"\n\n'
  printf '[theme.templates.user.hook_test]\n'
  printf 'input_path = "%s"\n' "$RUNTIME/hook_input.tmpl"
  printf 'output_path = "%s"\n' "$RUNTIME/hook_output.txt"
  printf 'post_hook = "sleep 1; touch %s/hook-done"\n' "$OUT"
} > "$RUNTIME/home/.config/noctalia/config.toml"

with_noctalia '
  noctalia_pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  [[ -n $noctalia_pid ]] || { echo "FAIL: noctalia not running"; exit 1; }
  # The boot-time theme apply runs the same hook; let it finish so neither its thread nor its marker counts.
  for _ in $(seq 50); do [[ -f "$OUT/hook-done" ]] && break; sleep 0.1; done
  sleep 0.3
  rm -f "$OUT/hook-done"
  threads_before=$(awk "/^Threads:/ {print \$2}" "/proc/$noctalia_pid/status")
  "$NOCTALIA" msg theme-mode-set light > /dev/null # default mode is dark; light guarantees an actual change

  threads_during=0
  ok=0
  for _ in $(seq 100); do
    [[ -f "$OUT/hook-done" ]] && { ok=1; break; }
    t=$(awk "/^Threads:/ {print \$2}" "/proc/$noctalia_pid/status")
    (( t > threads_during )) && threads_during=$t
    sleep 0.1
  done
  [[ $ok -eq 1 ]] || { echo "FAIL: post_hook marker never appeared"; tail -n 30 "$OUT/noctalia.log"; exit 1; }

  echo "threads_before=$threads_before threads_during=$threads_during" | tee "$OUT/thread-count.txt"
  (( threads_during <= threads_before )) \
    || { echo "FAIL: thread count grew while the hook ran ($threads_before -> $threads_during)"; exit 1; }
'

grep -q "^threads_before=" "$OUT/thread-count.txt" || { echo "FAIL: no thread-count recorded"; exit 1; }
echo "PASS: post_hook callback fired with no thread growth; $(cat "$OUT/thread-count.txt"); artifacts: $OUT"
