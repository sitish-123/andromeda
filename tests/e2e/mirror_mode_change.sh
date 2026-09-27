#!/usr/bin/env bash
# HEADLESS-2 mirrors HEADLESS-1. Changing the mirror's own output state commits a frame drawn from its off-layout
# scene, which is black; the mirror must copy the source's last frame again right after, not wait for the source to
# redraw. A headless source redraws after these changes anyway (gamma re-upload, the reload banner), so an LD_PRELOAD
# shim fails the source's own frame commits meanwhile, standing in for a source with nothing new to show. Covered
# twice: a transform applied through output management, and a mode set by config reload. The mirror has no wl_output
# to capture, so the probe is the shim's commit log: each change must be followed by a buffer-only commit on the
# mirror, which only a copy of the source makes. Writes mode.txt and the compositor log to $OUT (default
# ./artifacts/mirror-mode-change).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-mode-change}
source "$(dirname "$0")/lib.sh"
gcc -shared -fPIC -DWLR_USE_UNSTABLE $(pkg-config --cflags wlroots-0.20) -o "$RUNTIME/commit-fail.so" \
  "$(dirname "$0")/commit_fail_shim.c" -ldl
echo 0 > "$RUNTIME/fail-count"
LD_PRELOAD=$RUNTIME/commit-fail.so FAIL_COMMIT_OUTPUT=HEADLESS-1 FAIL_COMMIT_FILE=$RUNTIME/fail-count boot_headless 2
SRC=$ROOT/shell
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/proto.o" "$BUILD/wlr-output-management-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/output-transform" \
  "$(dirname "$0")/output_transform.cpp" "$SRC/src/wayland/output_management.cpp" "$RUNTIME/proto.o" -lwayland-client
: > "$OUT/mode.txt"
commits() { run "$UMBRIEL" output-commits --json | jq ".\"${1:-HEADLESS-2}\""; }
copies_since() { tail -n +"$(($1 + 1))" "$OUT/umbriel.log" | grep -c '^commit-shim: HEADLESS-2 committed=0x1$' || true; }
check() {
  local label=$1 from=$2 source_before=$3 copies
  sleep 0.5 # real time: a copy scheduled after the change lands within a frame
  copies=$(copies_since "$from")
  echo "source commits meanwhile: $(($(commits HEADLESS-1) - source_before)), none of them frames" >> "$OUT/mode.txt"
  echo 0 > "$RUNTIME/fail-count"
  sleep 0.3 # real time: the source's retried frame and the mirror's copy of it land before the next case
  (( copies >= 1 )) && echo "PASS $label: the mirror copied the source $copies time(s) after it" >> "$OUT/mode.txt" \
    || echo "FAIL $label: the mirror kept the black frame of the change" >> "$OUT/mode.txt"
}

printf '\n[output.HEADLESS-2]\nmirror = "HEADLESS-1"\n' >> "$RUNTIME/umbriel.toml"
run "$UMBRIEL" msg config-reload > /dev/null
for _ in $(seq 50); do grep -q "output 'HEADLESS-2': mirroring" "$OUT/umbriel.log" && break; sleep 0.1; done
run "$UMBRIEL" settle > /dev/null
sleep 0.5 # real time: the first copy lands

from=$(wc -l < "$OUT/umbriel.log") source_before=$(commits HEADLESS-1)
echo 1000000 > "$RUNTIME/fail-count"
run "$RUNTIME/output-transform" HEADLESS-2 1 >> "$OUT/mode.txt"
check "transform through output management" "$from" "$source_before"

from=$(wc -l < "$OUT/umbriel.log") source_before=$(commits HEADLESS-1)
echo 1000000 > "$RUNTIME/fail-count"
printf 'mode = "1024x768"\n' >> "$RUNTIME/umbriel.toml"
run "$UMBRIEL" msg config-reload > /dev/null
for _ in $(seq 50); do grep -q "output 'HEADLESS-2': applied mode=1024x768" "$OUT/umbriel.log" && break; sleep 0.1; done
check "mode through config reload" "$from" "$source_before"

cat "$OUT/mode.txt"
! grep -q '^FAIL' "$OUT/mode.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
