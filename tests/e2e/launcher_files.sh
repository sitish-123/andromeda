#!/usr/bin/env bash
# Launcher Files section end to end: andromeda-index serves a private home; a file written after the daemon started
# is found from the launcher by a word inside within 5 s and by name within 1 s of the write, and Enter opens it with
# its default app. Artifacts in $OUT
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
  : > "$OUT/result.txt"

  file=$HOME/Documents/zebra-ledger-2026.txt
  # find_file QUERY WITHIN_NS: open the launcher on QUERY until the index answers with one file. The daemon indexes a write
  # 200 ms after its last event (text later, on its idle worker); a launcher opened sooner sees nothing and opens again.
  find_file() {
    answers() { grep -c "files for \"$1\"" "$OUT/noctalia.log" || true; }
    while true; do
      before=$(answers "$1")
      "$NOCTALIA" msg panel-open launcher "$1" > /dev/null
      wait_for "the launcher got no answer from the index" "(( \$(answers \"$1\") > before ))"
      grep "files for \"$1\"" "$OUT/noctalia.log" | tail -1 | grep -q "\] 1 files" && break
      (( ($(date +%s%N) - written) < $2 )) || { echo "FAIL: \"$1\" was not found in time"; exit 1; }
      "$NOCTALIA" msg panel-close > /dev/null
    done
    echo "\"$1\" found $(( ($(date +%s%N) - written) / 1000000 )) ms after the write" | tee -a "$OUT/result.txt"
  }

  printf "the quokkaberry harvest\n" > "$HOME/Documents/harvest-notes.txt"
  written=$(date +%s%N)
  find_file quokkaberry 5000000000
  "$NOCTALIA" msg panel-close > /dev/null

  printf "totals\n" > "$file"
  written=$(date +%s%N)
  find_file zebra-ledger 1000000000

  wtype -k Return
  wait_for "Enter did not open the file" "[[ -s \"$RUNTIME/opened\" ]]"
  [[ $(cat "$RUNTIME/opened") == "$file" ]] || { echo "FAIL: opened $(cat "$RUNTIME/opened")"; exit 1; }
'
echo "PASS; artifacts: $OUT"
