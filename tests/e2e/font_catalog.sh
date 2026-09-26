#!/usr/bin/env bash
# The font family list behind Settings > Appearance comes from fontconfig in-process: opening the page logs the
# family count, which must match `fc-list : family` within 5 %, without running fc-list (a recording stub shadows it
# on PATH), and the shell's memory stays flat. Writes result.txt to $OUT (default ./artifacts/font-catalog).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/font-catalog}
source "$(dirname "$0")/lib.sh"
boot_headless 1

expected=$(fc-list : family | tr ',' '\n' | sed 's/^ *//; s/ *$//' | grep -v '^$' | sort -u | wc -l)
mkdir -p "$RUNTIME/stub"
printf '#!/bin/sh\necho "fc-list $*" >> "%s/spawned.txt"\n' "$OUT" > "$RUNTIME/stub/fc-list"
chmod +x "$RUNTIME/stub/fc-list"
: > "$OUT/spawned.txt"
export PATH="$RUNTIME/stub:$PATH"

with_noctalia '
  rss() { awk "/VmRSS/ {print \$2}" /proc/$(pgrep -f "^$NOCTALIA\$" | head -1)/status; }
  "$NOCTALIA" msg settings-open appearance > /dev/null
  for _ in $(seq 100); do grep -q "font catalog:" "$OUT/noctalia.log" && break; sleep 0.1; done
  first=$(rss); sleep 3; second=$(rss) # real time: memory must not grow once the page is built
  echo "rss_kb first=$first second=$second" > "$OUT/result.txt"
  (( second - first < 20000 && second < 400000 )) || { echo "FAIL: shell memory grew: $first -> $second"; exit 1; }
'
got=$(sed -n 's/.*font catalog: \([0-9]*\) families.*/\1/p' "$OUT/noctalia.log" | head -1)
echo "families=$got fc-list=$expected" | tee -a "$OUT/result.txt"
[[ -n $got ]] || { echo "FAIL: the catalog never loaded"; exit 1; }
(( got * 100 >= expected * 95 && got * 100 <= expected * 105 )) || { echo "FAIL: family count differs"; exit 1; }
[[ ! -s $OUT/spawned.txt ]] || { echo "FAIL: spawned $(cat "$OUT/spawned.txt")"; exit 1; }
echo "PASS; artifacts: $OUT"
