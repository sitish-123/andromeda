#!/usr/bin/env bash
# HEADLESS-2 mirrors HEADLESS-1 while an LD_PRELOAD shim fails the mirror's next buffer commit, as an EBUSY page flip
# does on the laptop panel. One pointer motion makes the source draw one frame; the mirror must retry its failed copy
# instead of staying stale until the source draws again, so it commits at least as often as the source. Writes
# retry.txt to $OUT (default ./artifacts/mirror-retry) plus the compositor log.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-retry}
source "$(dirname "$0")/lib.sh"
gcc -shared -fPIC -DWLR_USE_UNSTABLE $(pkg-config --cflags wlroots-0.20) -o "$RUNTIME/commit-fail.so" \
  "$(dirname "$0")/commit_fail_shim.c" -ldl
echo 0 > "$RUNTIME/fail-count"
LD_PRELOAD=$RUNTIME/commit-fail.so FAIL_COMMIT_OUTPUT=HEADLESS-2 FAIL_COMMIT_FILE=$RUNTIME/fail-count boot_headless 2
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

commits() { run "$UMBRIEL" output-commits --json | jq -r ".\"$1\""; }
run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
run "$POINTER" 1920 1080 move 100 100 > /dev/null
run "$UMBRIEL" settle > /dev/null
sleep 0.5 # real time: the mirror copies the settled frame

source_before=$(commits HEADLESS-1)
mirror_before=$(commits HEADLESS-2)
echo 1 > "$RUNTIME/fail-count"
run "$POINTER" 1920 1080 move 400 400 > /dev/null
sleep 0.5 # real time: the retry fires 16 ms after the failed commit
source_frames=$(($(commits HEADLESS-1) - source_before))
mirror_frames=$(($(commits HEADLESS-2) - mirror_before))
{
  echo "source frames: $source_frames, mirror commits: $mirror_frames, failures left to inject: $(< "$RUNTIME/fail-count")"
  [[ $(< "$RUNTIME/fail-count") == 0 ]] && echo "PASS a mirror commit failed" || echo "FAIL no mirror commit was attempted"
  (( source_frames > 0 )) && echo "PASS the source drew" || echo "FAIL the source did not draw"
  (( mirror_frames > 0 && mirror_frames >= source_frames )) && echo "PASS the mirror retried its failed copy" \
    || echo "FAIL the mirror stayed stale after its commit failed"
} | tee "$OUT/retry.txt"
! grep -q '^FAIL' "$OUT/retry.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
