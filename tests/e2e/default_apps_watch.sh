#!/usr/bin/env bash
# Settings > Default Apps rebuilds when mimeapps.list changes under it (inotify, not a per-second mtime poll): with
# the page open, an idle 3 s rebuilds nothing, and an external atomic replace of mimeapps.list rebuilds the page
# within 2 s. Writes result.txt to $OUT (default ./artifacts/default-apps-watch).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/default-apps-watch}
source "$(dirname "$0")/lib.sh"
boot_headless 1
export NOCTALIA_PROFILE=1

with_noctalia '
  rebuilds() { grep -c "profile prepareFrame rebuildContent:" "$OUT/noctalia.log" || true; }
  "$NOCTALIA" msg settings-open default-apps > /dev/null
  for _ in $(seq 100); do (( $(rebuilds) > 0 )) && break; sleep 0.1; done
  sleep 1; opened=$(rebuilds)
  sleep 3; idle=$(rebuilds) # real time: an idle page must not rebuild
  list=$RUNTIME/home/.config/mimeapps.list
  printf "[Default Applications]\nx-scheme-handler/http=e2e.desktop\n" > "$list.tmp" && mv "$list.tmp" "$list"
  changed=$idle
  for _ in $(seq 20); do changed=$(rebuilds); (( changed > idle )) && break; sleep 0.1; done
  echo "rebuilds opened=$opened idle=$idle after_write=$changed" | tee "$OUT/result.txt"
  (( opened > 0 )) || { echo "FAIL: the page never built"; exit 1; }
  (( idle == opened )) || { echo "FAIL: the idle page rebuilt"; exit 1; }
  (( changed > idle )) || { echo "FAIL: no rebuild after mimeapps.list changed"; exit 1; }
'
echo "PASS; artifacts: $OUT"
