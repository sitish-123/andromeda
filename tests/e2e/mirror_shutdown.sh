#!/usr/bin/env bash
# The compositor must shut down cleanly while an output mirrors another, whichever of the two was created first:
# HEADLESS-2 mirroring HEADLESS-1 (source destroyed first) and HEADLESS-1 mirroring HEADLESS-2, each ended once by
# SIGTERM and once by session-quit. Writes shutdown.txt and one compositor log per case to $OUT (default
# ./artifacts/mirror-shutdown).
set -euo pipefail
BASE=${OUT:-$(pwd)/artifacts/mirror-shutdown}
OUT=$BASE
source "$(dirname "$0")/lib.sh"
mkdir -p "$BASE"
: > "$BASE/shutdown.txt"

for target_source in "HEADLESS-2 HEADLESS-1" "HEADLESS-1 HEADLESS-2"; do
  for stop in term quit; do
    read -r target source <<< "$target_source"
    name="$target-mirrors-$source-$stop"
    OUT=$BASE/$name
    rm -f "$RUNTIME"/wayland-0*
    boot_headless 2
    pid=$!
    run "$DESKTOP_CLIENT" mirror "$target" "$source"
    for _ in $(seq 50); do grep -q "output '$target': mirroring" "$OUT/umbriel.log" && break; sleep 0.1; done
    run "$UMBRIEL" settle > /dev/null
    if [[ $stop == term ]]; then
      kill -TERM "$pid"
    else
      run "$UMBRIEL" msg session-quit:skip-confirmation > /dev/null 2>&1 || true
    fi
    status=0
    wait "$pid" 2> /dev/null || status=$?
    if (( status == 0 )) && ! grep -qiE 'abort|assert|double free|free\(\)|sanitizer' "$OUT/umbriel.log"; then
      echo "PASS $name: exit status 0" >> "$BASE/shutdown.txt"
    else
      echo "FAIL $name: exit status $status; $(grep -iE 'abort|assert|free' "$OUT/umbriel.log" | tail -1)" >> "$BASE/shutdown.txt"
    fi
  done
done

cat "$BASE/shutdown.txt"
! grep -q '^FAIL' "$BASE/shutdown.txt" || { echo "FAIL; artifacts: $BASE"; exit 1; }
echo "PASS; artifacts: $BASE"
