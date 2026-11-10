#!/usr/bin/env bash
# Control-center Notifications page: an active notification with an action shows its action button (it did not:
# buttons showed only for expired ones), and clicking it sends ActionInvoked to the app. Every step waits on state
# with a timeout. A click on the card body (not a button) sends the "default" action. Shots go to $OUT (default ./artifacts/notifications-tab-actions).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/notifications-tab-actions}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }
  settle() {
    local i
    for i in $(seq 25); do
      grim "$OUT/prev.png"; sleep 0.2; grim "$OUT/now.png"
      cmp -s "$OUT/prev.png" "$OUT/now.png" && return 0
    done
    fail "the screen did not settle"
  }
  tag=e2e-act-$$-$RANDOM
  # notify-send -w stays connected and prints the invoked action key: that is the app side of ActionInvoked.
  notify-send -w -t 0 -a "$tag" -A "reply=Reply $tag" "Message $tag" "body" > "$OUT/invoked.txt" 2>&1 &
  "$NOCTALIA" msg panel-open control-center detail-notifications > /dev/null
  settle
  cp "$OUT/now.png" "$OUT/1-tab.png"
  # The row button (not the banner): the first action sits at the right end of the row.
  "$POINTER" 1280 720 move 1148 183 > /dev/null
  "$POINTER" 1280 720 move 1148 183 click 272 > /dev/null
  invoked() { grep -q "reply" "$OUT/invoked.txt"; }
  wait_for 5 invoked || fail "the row action did not reach the app (invoked.txt: $(cat "$OUT/invoked.txt"))"
  # The action closed the panel and dismissed its notification, so the new one fills the same row: a click on
  # its summary line runs "default".
  notify-send -w -t 0 -a "$tag-d" -A "default=Open" "Open $tag" "body" > "$OUT/default.txt" 2>&1 &
  "$POINTER" 1280 720 move 5 715 > /dev/null
  "$NOCTALIA" msg panel-open control-center detail-notifications > /dev/null
  settle
  cp "$OUT/now.png" "$OUT/2-default.png"
  "$POINTER" 1280 720 move 986 183 > /dev/null
  "$POINTER" 1280 720 move 986 183 click 272 > /dev/null
  defaulted() { grep -q "default" "$OUT/default.txt"; }
  wait_for 5 defaulted || fail "a card body click did not send default (default.txt: $(cat "$OUT/default.txt"))"
  echo PASS
'
