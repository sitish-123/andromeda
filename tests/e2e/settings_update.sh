#!/usr/bin/env bash
# Settings › Software Update against mock Update1 + UpdateHelper1 (tests/e2e/mock_update.py): the news-gated page,
# a click on "I Have Read This" (helper then session MarkNewsRead), a click on Update Now (Upgrade false, progress,
# Finished, re-check), and an upgrade another client starts (running, restart needed). Light/dark screenshots in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-update}
source "$(dirname "$0")/lib.sh"
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
: > "$OUT/calls.txt"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_update.py"
export MOCK_NEWS=1
# A stub flatpak: Update Now must update Apps as the user, and the test must never touch real installations.
mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho "flatpak $*" >> "%s"\n' "$OUT/calls.txt" > "$OUT/bin/flatpak"
chmod +x "$OUT/bin/flatpak"
BEFORE_NOCTALIA='
  export PATH="$OUT/bin:$PATH"
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done
  grep -q ready "$OUT/mock.log" || { echo "FAIL: mock update did not start"; exit 1; }
' with_noctalia '
  wait_calls() {  # wait_calls PATTERN COUNT: the mock logged PATTERN at least COUNT times (5 s)
    for _ in $(seq 50); do [ "$(grep -c "$1" "$OUT/calls.txt" || true)" -ge "$2" ] && return 0; sleep 0.1; done
    echo "FAIL: mock never saw $1 x$2"; cat "$OUT/calls.txt"; exit 1
  }
  helper() { dbus-send --session --type=method_call --print-reply --dest="$1" "$2" "$3" "${@:4}" > /dev/null; }
  "$NOCTALIA" msg settings-open software-update > /dev/null
  wait_calls GetAll 1
  for mode in light dark; do
    "$NOCTALIA" msg theme-mode-set "$mode" > /dev/null
    sleep 0.5
    grim "$OUT/update-news-$mode.png"
  done
  # Positions on the 1280x720 headless output (screenshots in $OUT show them).
  "$POINTER" 1280 720 move 1003 170 click 272 > /dev/null   # I Have Read This
  wait_calls HelperMarkNewsRead 1
  wait_calls "^MarkNewsRead" 1
  wait_calls GetAll 2
  sleep 0.5
  grim "$OUT/update-available-dark.png"
  "$POINTER" 1280 720 move 1010 173 click 272 > /dev/null   # Update Now
  wait_calls "^Upgrade False" 1
  wait_calls "^flatpak update --noninteractive -y" 1
  wait_calls "^Check" 1
  sleep 0.5
  grim "$OUT/update-clicked-dark.png"
  helper org.andromeda.UpdateHelper1 /org/andromeda/UpdateHelper1 dsk.test.Update.EmitProgress string:install int32:40 string:mesa
  sleep 0.5
  grim "$OUT/update-running-dark.png"
  helper org.andromeda.UpdateHelper1 /org/andromeda/UpdateHelper1 org.andromeda.UpdateHelper1.Upgrade boolean:false
  wait_calls "^Check" 2
  sleep 0.5
  grim "$OUT/update-restart-dark.png"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  sleep 0.5
  grim "$OUT/update-restart-light.png"
'
grep -n "software-update" "$OUT/noctalia.log" | grep -i -E "warn|error" && { echo "FAIL: software-update warnings"; exit 1; }
echo PASS
