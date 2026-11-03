#!/usr/bin/env bash
# Launcher "Show all files": with 13 matching files the Files section shows 8 and a "Show all files" row; Enter on it
# opens the Files scope, which lists all 13; Tab twice picks the PDFs chip (1 file); Backspace on the empty field
# leaves the scope. Captures in $OUT (default ./artifacts/launcher-files-show-all): sections, scope, kind, left.png.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/launcher-files-show-all}
source "$(dirname "$0")/lib.sh"
INDEX=${INDEX:-$ROOT/indexer/build/andromeda-index}
boot_headless 1

home=$RUNTIME/home
mkdir -p "$home/Documents"
for i in $(seq -w 1 12); do
  printf 'kiwi %s\n' "$i" > "$home/Documents/kiwi-report-$i.txt"
  touch -d "2026-10-$i 12:00" "$home/Documents/kiwi-report-$i.txt"
done
printf '%%PDF-1.4\n' > "$home/Documents/kiwi-report-scan.pdf"

export NOCTALIA_LOG_LEVEL=debug INDEX
with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  "$INDEX" serve > "$OUT/index.log" 2>&1 &
  wait_for "the index daemon did not start" "grep -q watching \"$OUT/index.log\""
  wtype -s 3600000 &

  "$NOCTALIA" msg panel-open launcher kiwi-report > /dev/null
  wait_for "the Files section did not get 8 files" "grep -q \"\] 8 files for \\\"kiwi-report\\\"\$\" \"$OUT/noctalia.log\""
  sleep 0.4
  grim "$OUT/sections.png"

  # 8 file rows, then "Show all files".
  for _ in $(seq 8); do wtype -k Down; done
  wtype -k Return
  wait_for "the Files scope did not list 13 files" \
    "grep -q \"\] 13 files for \\\"kiwi-report\\\" in the Files scope\$\" \"$OUT/noctalia.log\""
  sleep 0.4
  grim "$OUT/scope.png"

  # Kind chips: All, Folders, PDFs.
  wtype -k Tab
  wtype -k Tab
  wait_for "the PDFs chip did not filter to 1 file" \
    "grep -q \"\] 1 files for \\\"kiwi-report\\\" in the Files scope kind pdf\" \"$OUT/noctalia.log\""
  sleep 0.4
  grim "$OUT/kind.png"

  for _ in $(seq 11); do wtype -k BackSpace; done
  sleep 0.2
  wtype -k BackSpace
  sleep 1.5
  grim "$OUT/left.png"
'
echo "PASS; artifacts: $OUT"
