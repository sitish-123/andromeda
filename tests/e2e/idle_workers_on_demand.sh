#!/usr/bin/env bash
# The calendar and wallpaper-scan workers start with work and exit after 30 s idle. A vdir calendar with one event
# today and a directory of three wallpapers are configured: the calendar tab shows the event (read back with
# tesseract), the wallpaper panel lists the images, and 40 s after both panels close the shell is down to its main
# thread among unnamed "noctalia" threads; a fourth image added then is found by a fresh worker. Writes calendar.png,
# calendar.txt, wallpaper.png, wallpaper-rescan.png, and threads.txt to $OUT
# (default ./artifacts/idle-workers-on-demand).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/idle-workers-on-demand}
source "$(dirname "$0")/lib.sh"
boot_headless 1
rm -f "$OUT"/*.png "$OUT"/*.txt

CAL=$RUNTIME/calendars/personal
mkdir -p "$CAL" "$RUNTIME/walls"
echo "Personal" > "$CAL/displayname"
START=$(date -u -d "+2 hours" +%Y%m%dT%H0000Z)
END=$(date -u -d "+3 hours" +%Y%m%dT%H0000Z)
printf 'BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//e2e//EN\r\nBEGIN:VEVENT\r\nUID:e2e-1@local\r\nDTSTAMP:%s\r\nDTSTART:%s\r\nDTEND:%s\r\nSUMMARY:Quarterly Pelican Review\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n' \
  "$START" "$START" "$END" > "$CAL/e2e-1.ics"
for colour in red green blue; do magick -size 640x360 "xc:$colour" "$RUNTIME/walls/$colour.png"; done
cat >> "$RUNTIME/home/.config/noctalia/config.toml" <<TOML

[calendar]
enabled = true

[calendar.account.local]
type = "vdir"
path = "$RUNTIME/calendars"

[wallpaper]
directory = "$RUNTIME/walls"
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  unnamed() { cat /proc/$pid/task/*/comm | grep -cx noctalia; }

  msg panel-open control-center calendar > /dev/null
  sleep 2 # real time: the vdir load and the first paint
  grim "$OUT/calendar.png"
  magick "$OUT/calendar.png" -resize 200% "$OUT/calendar-ocr.png"
  tesseract "$OUT/calendar-ocr.png" - 2> /dev/null > "$OUT/calendar.txt"
  rm "$OUT/calendar-ocr.png"
  grep -q "Pelican" "$OUT/calendar.txt" || fail "calendar event not shown: $(tr "\n" " " < "$OUT/calendar.txt")"
  msg panel-close > /dev/null

  msg panel-open wallpaper > /dev/null
  sleep 2 # real time: the directory scan and thumbnails
  busy=$(unnamed)
  grim "$OUT/wallpaper.png"
  msg panel-close > /dev/null

  sleep 40 # real time: past the 30 s idle exit
  idle=$(unnamed)
  { echo "busy=$busy idle=$idle"; cat /proc/$pid/task/*/comm | sort | uniq -c | sort -rn; } > "$OUT/threads.txt"
  (( busy > 1 )) || fail "no worker started while scanning (busy=$busy)"
  (( idle == 1 )) || fail "$idle unnamed threads idle, want the main thread only"

  magick -size 640x360 xc:yellow "$RUNTIME/walls/yellow.png"
  msg panel-open wallpaper > /dev/null
  sleep 2 # real time: a new worker rescans the changed directory
  grim "$OUT/wallpaper-rescan.png"
  tesseract "$OUT/wallpaper-rescan.png" - 2> /dev/null | grep -q "yellow" || fail "no rescan after the worker exited"
'
cat "$OUT/threads.txt"
echo "PASS; artifacts: $OUT"
