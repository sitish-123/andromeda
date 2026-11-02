#!/usr/bin/env bash
# harness: xcursor-theme=true
# Rulebook §10: a launch turns the arrow into the busy cursor until the app's window maps, or for at most 5 s.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly TOKEN_CLIENT="${ANDROMEDA_ACTIVATION_TOKEN_CLIENT:-./build-debug/tests/activation-token-client}"

if [[ ! -x $POINTER ]] || [[ ! -x $TOKEN_CLIENT ]] || ! command -v foot > /dev/null; then
  echo "cursor helpers are not available"
  exit 1
fi

xcursor() { "$ANDROMEDA" cursor-state --json | jq -r '.ok.xcursor // .xcursor'; }
# await_xcursor NAME TENTHS / await_not_busy TENTHS: poll the cursor name.
await_xcursor() {
  local want=$1 now=''
  for _ in $(seq "$2"); do
    now=$(xcursor)
    [[ $now == "$want" ]] && return 0
    sleep 0.1
  done
  echo "cursor is '$now', expected '$want'"
  exit 1
}
# The mapped window may sit under the pointer and set its own cursor, so the end of busy is "not progress".
await_not_busy() {
  for _ in $(seq "$1"); do
    [[ $(xcursor) != progress ]] && return 0
    sleep 0.1
  done
  echo "cursor is still busy"
  exit 1
}

# Pointer over the empty desktop: the compositor's arrow.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 640 360
await_xcursor default 30

# A spawn keybind (no app id hint) is not a launch: no busy cursor.
"$ANDROMEDA" msg spawn sleep 30 > /dev/null
for _ in $(seq 5); do
  [[ $(xcursor) == default ]] || { echo "a plain spawn turned the cursor busy"; exit 1; }
  sleep 0.1
done

# A launch (token with an app id hint, as the dock and launcher request) whose window maps after 0.5 s:
# busy at once, the arrow again once the window maps.
token=$("$TOKEN_CLIENT" busy-launch)
(sleep 0.5; XDG_ACTIVATION_TOKEN=$token exec foot --config=/dev/null --title=busy-launch sh -c "sleep 1") \
  > /dev/null 2>&1 &
await_xcursor progress 5
await_not_busy 50
"$ANDROMEDA" windows --json | jq -e 'any(.[]; .title == "busy-launch")' > /dev/null \
  || { echo "the busy cursor ended before the window mapped"; exit 1; }

# A launch that never maps a window: busy, then the arrow after the 5 s limit.
for _ in $(seq 100); do
  "$ANDROMEDA" windows --json | jq -e 'any(.[]; .title == "busy-launch")' > /dev/null || break
  sleep 0.1
done
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 640 360
await_xcursor default 30
"$TOKEN_CLIENT" never-maps > /dev/null
await_xcursor progress 5
start=$(date +%s%N)
await_not_busy 80
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
(( elapsed_ms >= 3500 )) || { echo "busy cursor ended after ${elapsed_ms} ms, before the 5 s limit"; exit 1; }
echo "PASS: busy cursor until map, and until the 5 s limit without a window (${elapsed_ms} ms)"
