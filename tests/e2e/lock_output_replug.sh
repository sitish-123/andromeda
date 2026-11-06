#!/usr/bin/env bash
# No wrong password: the headless shell checks it with the real PAM login stack, and faillock counts it.
# The lock screen survives losing every output, as on a VT switch (wlroots destroys the outputs on pause and creates
# them again on resume): lock, destroy the only output, create it again, then the lock screen draws on it (not the
# compositor's black lock blank), typing redraws it,. Writes back.png and
# noctalia.log to $OUT (default ./artifacts/lock-output-replug).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-output-replug}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
cat > "$RUNTIME/home/.config/noctalia/config.toml" <<'TOML'
[shell]
setup_wizard_enabled = false
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; grim "$OUT/fail.png" || true; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }
  not_black() { grim -t ppm - | tail -c 30000 | od -An -tu1 -v | awk "{ for (i = 1; i <= NF; i++) s += \$i } END { exit !(s > 0) }"; }
  shot_differs() { grim -t ppm "$OUT/now.ppm" && ! cmp -s "$OUT/now.ppm" "$1"; }

  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wait_for "the lock screen did not draw" not_black

  "$ANDROMEDA" output-destroy HEADLESS-1 > /dev/null || fail "output-destroy failed"
  sleep 1
  "$ANDROMEDA" output-create HEADLESS-1 > /dev/null || fail "output-create failed"
  locked || fail "losing the output unlocked the session"
  wait_for "the lock screen did not draw on the re-created output (black)" not_black
  sleep 1
  grim -t ppm "$OUT/back.ppm"
  grim "$OUT/back.png"

  wtype abcdef
  wait_for "the lock screen did not redraw after typing" shot_differs "$OUT/back.ppm"
  locked || fail "the session unlocked"
  echo "PASS"
'
