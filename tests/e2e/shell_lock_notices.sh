#!/usr/bin/env bash
# org.andromeda.Shell1.LockNotices: the unseen notification groups for the `noctalia lock` process. Two notifications
# from one app and one from another give two groups (counts 2 and 1, newest first) with no body text.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/shell-lock-notices}
mkdir -p "$OUT"
export OUT
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }
  get() { busctl --user get-property org.andromeda.Shell1 /org/andromeda/Shell1 org.andromeda.Shell1 LockNotices; }
  tag=e2e-ln-$$-$RANDOM
  wait_for 10 get > /dev/null || fail "Shell1 has no LockNotices"
  notify-send -a "$tag-a" "One $tag" "secret-body-$tag"
  notify-send -a "$tag-a" "Two $tag" "secret-body-$tag"
  notify-send -a "$tag-b" "Three $tag" "secret-body-$tag"
  grouped() { get | grep -q "\"$tag-b\" \"\" 1 [0-9]* \"$tag-a\" \"\" 2 "; }
  wait_for 5 grouped || fail "groups missing: $(get)"
  get | grep -q "secret-body" && fail "body text leaked: $(get)"
  echo "LockNotices: $(get)"
  echo PASS
'
