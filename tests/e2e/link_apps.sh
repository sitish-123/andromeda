#!/usr/bin/env bash
# Phone apps in desktop windows: umbriel-link-apps against an emulator adb already reaches (as wireless debugging does
# for a paired phone), inside a headless Umbriel. Proves: --list names the launcher apps (Settings among them);
# --launch opens Settings on a new virtual display, not the phone's own, in a scrcpy window the compositor shows; and
# closing that window removes the display. Needs scrcpy and a running emulator (ANDROID_SERIAL, default
# emulator-5554). Writes results.jsonl, apps.jsonl, window.png, scrcpy.log, umbriel.log to $OUT (default
# ./artifacts/link-apps).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-apps}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
export ANDROID_SERIAL=${ANDROID_SERIAL:-emulator-5554}
rm -rf "$OUT"
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
command -v scrcpy > /dev/null || fail "scrcpy is not installed"
[[ $(adb get-state 2> /dev/null) == device ]] || fail "no emulator at $ANDROID_SERIAL"
boot_headless 1
virtual() { adb shell dumpsys display | grep -c 'mDisplayId=.*scrcpy\|"scrcpy"' || true; }

"$LINK_BIN/umbriel-link-apps" any --list > "$OUT/apps.jsonl" 2> "$OUT/scrcpy.log" || fail "--list: $(tail -3 "$OUT/scrcpy.log")"
grep -q '"package":"com.android.settings"' "$OUT/apps.jsonl" || fail "Settings is not listed: $(head -5 "$OUT/apps.jsonl")"
record "{\"step\":\"lists-apps\",\"count\":$(wc -l < "$OUT/apps.jsonl")}"

BEFORE=$(virtual)
run env SDL_VIDEODRIVER=wayland "$LINK_BIN/umbriel-link-apps" any --launch com.android.settings >> "$OUT/scrcpy.log" 2>&1 \
  || fail "--launch failed"
for _ in $(seq 150); do (( $(virtual) > BEFORE )) && break; sleep 0.1; done
(( $(virtual) > BEFORE )) || fail "no scrcpy virtual display appeared"
DISPLAY_ID=$(adb shell dumpsys activity activities | grep -B30 'com.android.settings' | grep -o 'displayId=[1-9][0-9]*' | head -1)
[[ -n $DISPLAY_ID ]] || fail "Settings is not on a separate display"
sleep 2 # real time: the window maps and paints its first frame
run grim "$OUT/window.png"
COLOURS=$(magick "$OUT/window.png" -format '%k' info:)
(( COLOURS > 50 )) || fail "the window shows no app ($COLOURS colours)"
record "{\"step\":\"launches-on-own-display\",\"display\":\"${DISPLAY_ID#displayId=}\",\"colours\":$COLOURS}"

pkill -f -- '--start-app=com.android.settings' || true
for _ in $(seq 100); do (( $(virtual) <= BEFORE )) && break; sleep 0.1; done
(( $(virtual) <= BEFORE )) || fail "closing the window left its display behind"
record '{"step":"closing-removes-display"}'
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
