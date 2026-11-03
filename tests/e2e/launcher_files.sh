#!/usr/bin/env bash
# Launcher Files section end to end: andromeda-index serves a private home; a file written after the daemon started
# is found from the launcher within 1 s of the write, and Enter opens it with its default app. Artifacts in $OUT
# (default ./artifacts/launcher-files).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/launcher-files}
source "$(dirname "$0")/lib.sh"
INDEX=${INDEX:-$ROOT/indexer/build/andromeda-index}
boot_headless 1

# A test default app for text/plain records the file it was asked to open.
home=$RUNTIME/home
mkdir -p "$home/Documents" "$home/.local/share/applications"
printf '#!/bin/sh\nprintf "%%s\\n" "$1" > "%s/opened"\n' "$RUNTIME" > "$RUNTIME/open-file"
chmod +x "$RUNTIME/open-file"
printf '[Desktop Entry]\nType=Application\nName=E2E Open\nExec=%s %%f\nMimeType=text/plain;\n' "$RUNTIME/open-file" \
  > "$home/.local/share/applications/e2e-open.desktop"
printf '[Default Applications]\ntext/plain=e2e-open.desktop\n' > "$home/.config/mimeapps.list"

export NOCTALIA_LOG_LEVEL=debug INDEX
with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  "$INDEX" serve > "$OUT/index.log" 2>&1 &
  wait_for "the index daemon did not start" "grep -q watching \"$OUT/index.log\""
  wtype -s 3600000 &

  file=$HOME/Documents/zebra-ledger-2026.txt
  printf "totals\n" > "$file"
  written=$(date +%s%N)
  # The daemon indexes a write 200 ms after its last event; a launcher opened sooner sees no file and opens again.
  answers() { grep -c "files for \"zebra-ledger\"" "$OUT/noctalia.log" || true; }
  while true; do
    before=$(answers)
    "$NOCTALIA" msg panel-open launcher "zebra-ledger" > /dev/null
    wait_for "the launcher got no answer from the index" "(( \$(answers) > before ))"
    grep "files for \"zebra-ledger\"" "$OUT/noctalia.log" | tail -1 | grep -q "\] 1 files" && break
    (( ($(date +%s%N) - written) < 1000000000 )) || { echo "FAIL: the new file was not found within 1 s"; exit 1; }
    "$NOCTALIA" msg panel-close > /dev/null
  done
  found_ms=$(( ($(date +%s%N) - written) / 1000000 ))
  echo "found ${found_ms} ms after the write" | tee "$OUT/result.txt"
  ((found_ms < 1000)) || { echo "FAIL: found after ${found_ms} ms"; exit 1; }

  wtype -k Return
  wait_for "Enter did not open the file" "[[ -s \"$RUNTIME/opened\" ]]"
  [[ $(cat "$RUNTIME/opened") == "$file" ]] || { echo "FAIL: opened $(cat "$RUNTIME/opened")"; exit 1; }
'
echo "PASS; artifacts: $OUT"
