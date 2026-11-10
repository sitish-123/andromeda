#!/usr/bin/env bash
# org.andromeda.Shell1.LockNotices: the unseen notification groups for the `noctalia lock` process. Two notifications
# from one app and one from another give two groups (counts 2 and 1, newest first) with no body text.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/shell-lock-notices}
mkdir -p "$OUT"
export OUT
source "$(dirname "$0")/lib.sh"
boot_headless 1
NM_MOCK="$(cd "$(dirname "$0")" && pwd)/mock_nm.py"

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

  # Delegated: `noctalia lock` reads the same two groups from Shell1 and the glyph from NetworkManager, then locks.
  MOCK_NM_PRIMARY=802-3-ethernet python3 '"$NM_MOCK"' "$OUT/nm-calls.txt" > "$OUT/nm-mock.log" 2>&1 &
  mockpid=$!
  wait_for 5 grep -q ready "$OUT/nm-mock.log" || fail "the NetworkManager mock did not start"
  NOCTALIA_LOG_LEVEL=debug "$NOCTALIA" lock > "$OUT/lock.log" 2>&1 &
  lockpid=$!
  wait_for 10 grep -q "lock notices: 2 groups" "$OUT/lock.log" || fail "the lock process did not read 2 groups"
  wait_for 5 grep -q "network glyph ethernet$" "$OUT/lock.log" \
    || fail "network glyph is not ethernet: $(grep "network glyph" "$OUT/lock.log" | tail -n 1)"
  served() { busctl --user status org.andromeda.Lock1 > /dev/null 2>&1; }
  wait_for 5 served || fail "org.andromeda.Lock1 is not served"
  busctl --user call org.andromeda.Lock1 /org/andromeda/Lock1 org.andromeda.Lock1 Lock s user > /dev/null
  wait_for 5 grep -q "session is locked" "$OUT/andromeda.log" || fail "the lock process did not lock"
  kill "$lockpid" "$mockpid"; wait "$lockpid" "$mockpid" 2> /dev/null || true
  echo PASS
'
