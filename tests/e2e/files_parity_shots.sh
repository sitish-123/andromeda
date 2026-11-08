#!/usr/bin/env bash
# Files parity shots at output scale 2 (2560x1440, pixels 1:1 with .agent/mockups/files-v2-live.html at device scale
# 2), on the real shell, for the surfaces of FILES-UI-SPEC §8c: details pane and split view (§2, dark), Open With and
# the Merge sheet (§4, §5, dark), item and background menus and the conflict sheet (§4, §5, light), search (§5, dark).
# Shots only; the mockup is the reference. Fails when a surface does not open. Images in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-parity/live}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
INDEX=${INDEX:-$ROOT/indexer/build/andromeda-index}
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
mkdir -p "$OUT"

H=$RUNTIME/home
P=$H/Projects
mkdir -p "$P/Andromeda" "$P/Website" "$P/Thesis" "$P/Kathmandu" "$H/Pictures/Kathmandu" "$H/Camera" "$H/Movies" \
  "$H/Documents/Taxes 2026" "$H/.config"
echo x > "$P/Andromeda/roadmap.md"; echo x > "$P/budget.ods"; echo x > "$P/report.pdf"
echo new > "$P/Kathmandu/IMG_0412.jpg"; echo x > "$P/Kathmandu/notes.txt"
echo old-and-longer > "$H/Pictures/Kathmandu/IMG_0412.jpg"; echo x > "$H/Pictures/Kathmandu/IMG_0415.jpg"
echo x > "$H/Documents/Taxes 2026/invoice-2026-10.pdf"
# Real clips (when ffmpeg is there) so the conflict cards show thumbnails, durations and sizes: the copy is longer.
clip() { ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=320x180:rate=4" -t "$2" -c:v mpeg4 "$1" 2> /dev/null \
  || head -c $(($2 * 100)) /dev/zero > "$1"; }
clip "$H/Movies/holiday.mov" 42; clip "$H/Camera/holiday.mov" 38
clip "$H/Movies/trailer.mov" 12; clip "$H/Camera/trailer.mov" 12
touch -d "-5 weeks" "$H/Camera/holiday.mov"
python3 -I - "$P" <<'PY'
import sys, zlib, struct
def png(path, w, h, a, b):
    rows = []
    for y in range(h):
        t = y / max(1, h - 1)
        rows.append(b"\0" + bytes(int(a[i] + (b[i] - a[i]) * t) for i in range(3)) * w)
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
png(sys.argv[1] + "/launch-poster.png", 384, 216, (60, 50, 160), (220, 110, 70))
import os
os.setxattr(sys.argv[1] + "/launch-poster.png", "user.xdg.tags", b"Work,Done")
PY

export FILES FILEOPS POINTER INDEX H P
with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; }
  keys() { "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
  log() { cat "$OUT/files.log"; }
  mark() { wc -l < "$OUT/files.log"; }
  wait_log() { for _ in $(seq 100); do tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0; sleep 0.05; done; return 1; }
  action() { gdbus call --session -d dev.andromeda.Files -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" "{}" > /dev/null; }
  at() { local line; line=$(grep -E "geometry $1( |$)" "$OUT/files.log" | tail -n 1); [[ -n $line ]] || fail "no geometry for $1"
    read -r x y <<< "$(awk "{print \$(NF-1), \$NF}" <<< "$line")"; echo "$((WX + x)) $((WY + y))"; }
  shot() { sleep 0.4; settle; grim "$OUT/$1.png"; }
  "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null || true
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  HOME="$H" XDG_DATA_HOME="$H/.local/share" XDG_CONFIG_HOME="$H/.config" XDG_STATE_HOME="$H/.local/state" \
    "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
  HOME="$H" XDG_STATE_HOME="$H/.local/state" "$INDEX" serve > "$OUT/index.log" 2>&1 &
  INDEX_PID=$!
  for _ in $(seq 200); do grep -q watching "$OUT/index.log" && break; sleep 0.05; done
  grep -q watching "$OUT/index.log" || fail "the index daemon did not start"
  HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_STATE_HOME="$H/.local/state" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug \
    DESK_FILES_GEOMETRY=1 "$FILES" "$P" > "$OUT/files.log" 2>&1 &
  for _ in $(seq 100); do grep -q "first frame" "$OUT/files.log" 2> /dev/null && break; sleep 0.05; done
  grep -q "first frame" "$OUT/files.log" || fail "Files drew no first frame"
  w=$("$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"dev.andromeda.Files\")][0]")
  WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w")

  # §2 dark: list view, launch-poster.png selected, details pane, then split view.
  keys mod control tap 3 mod none
  m=$(mark); keys tap 38 tap 30 tap 22
  wait_log "$m" "cursor launch-poster.png" || fail "type-ahead did not reach launch-poster.png"
  keys mod alt tap 25 mod none
  wait_log "$m" "details pane on" || fail "Alt+P did not open the details pane"
  shot d-details
  m=$(mark); keys tap 61
  wait_log "$m" "split view on" || fail "F3 did not open the split view"
  shot d-split
  m=$(mark); keys tap 61
  wait_log "$m" "split view off" || fail "F3 did not close the split view"
  m=$(mark); keys mod alt tap 25 mod none
  wait_log "$m" "details pane off" || fail "Alt+P did not close the details pane"

  # §4 right, dark: Open With for report.pdf.
  m=$(mark); action select "[<[\"report.pdf\"]>]"
  wait_log "$m" "selected 1 of" || fail "could not select report.pdf"
  action file.open-with
  sleep 0.6 # the sheet fades in on spring.open (about 0.4 s); the shot waits it out
  shot d-open-with
  keys tap 1

  # §5 right, dark: Kathmandu onto Pictures/Kathmandu = the Merge sheet.
  m=$(mark); action select "[<[\"Kathmandu\"]>]"
  wait_log "$m" "selected 1 of" || fail "could not select Kathmandu"
  action edit.copy
  action go "[<\"file://$H/Pictures\">]"
  m=$(mark); action edit.paste
  wait_log "$m" "conflict sheet merge" || fail "the folder paste did not open the Merge sheet"
  shot d-merge
  m=$(mark); action conflict.skip
  wait_log "$m" "job copy done" || fail "Skip on the Merge sheet left the job running"

  # §4 left and §5 left, light: item menu, background menu, conflict sheet.
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  action go "[<\"file://$P\">]"
  keys mod control tap 2 mod none
  m=$(mark); wait_log 0 "geometry item report.pdf" || fail "no geometry for report.pdf"
  sleep 0.5; settle
  "$POINTER" 1280 720 move $(at "item report.pdf") click 273 > /dev/null
  shot l-item-menu
  keys tap 1
  "$POINTER" 1280 720 move $(at view) click 273 > /dev/null
  shot l-background-menu
  keys tap 1
  m=$(mark); action go "[<\"file://$H/Movies\">]"
  wait_log "$m" "location $H/Movies" || fail "go did not open Movies"
  m=$(mark); action select "[<[\"holiday.mov\", \"trailer.mov\"]>]"
  wait_log "$m" "selected 2 of" || fail "could not select holiday.mov and trailer.mov"
  action edit.copy
  action go "[<\"file://$H/Camera\">]"
  m=$(mark); action edit.paste
  wait_log "$m" "job copy conflict" || fail "the file paste raised no conflict"
  shot l-conflict
  m=$(mark); action conflict.skip
  wait_log "$m" "conflict sheet file" || fail "the second conflict did not ask"
  m=$(mark); action conflict.skip
  wait_log "$m" "job copy done" || fail "Skip on both conflicts left the copy running"

  # §5 search, dark: Ctrl+F, "invoice".
  m=$(mark); "$NOCTALIA" msg theme-mode-set dark > /dev/null
  wait_log "$m" "scheme dark" || fail "Files did not follow the dark scheme"
  action go "[<\"file://$H\">]"
  m=$(mark); action edit.find
  wait_log "$m" "search mode on" || fail "Ctrl+F did not start the search mode header"
  m=$(mark); keys tap 23 tap 49 tap 47 tap 24 tap 23 tap 46 tap 18
  wait_log "$m" "geometry chip This PC" || fail "the search chips did not show"
  sleep 0.3; settle
  m=$(mark); "$POINTER" 1280 720 move $(at "chip This PC") click 272 > /dev/null
  wait_log "$m" "search results [1-9]" || fail "This PC found nothing for invoice"
  wait_log "$m" "geometry item invoice-2026-10.pdf" || fail "This PC did not list invoice-2026-10.pdf"
  shot d-search
  kill "$INDEX_PID" 2> /dev/null || true # the index daemon outlives the shell otherwise, and the teardown waits on it
  echo "pass files_parity_shots"
'
