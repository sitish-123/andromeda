#!/usr/bin/env bash
# No wrong password: the headless shell checks it with the real PAM login stack, and faillock counts it.
# The lock screen survives screen-off: lock, power the outputs off, wake them with a key press (no system sleep), then
# typing redraws the password field. Writes woke.png, typed.png and
# noctalia.log to $OUT (default ./artifacts/lock-dpms-wake).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-dpms-wake}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
cat > "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'TOML'
[shell]
setup_wizard_enabled = false
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; grim "$OUT/fail.png" || true; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; fail "$what"; }
  log_since() { tail -n +"$(($1 + 1))" "$OUT/andromeda.log" | grep -q "$2"; }
  shot_differs() { grim -t ppm "$OUT/now.ppm" && ! cmp -s "$OUT/now.ppm" "$1"; }

  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  sleep 1

  mark=$(wc -l < "$OUT/andromeda.log")
  msg dpms-off > /dev/null
  wait_for "the output did not power off" log_since "$mark" "powered off"
  sleep 1

  mark=$(wc -l < "$OUT/andromeda.log")
  wtype -k Shift_L
  wait_for "a key press did not wake the output" log_since "$mark" "applied mode="
  sleep 1
  grim -t ppm "$OUT/woke.ppm"
  grim "$OUT/woke.png"

  wtype abcdef
  wait_for "the lock screen did not redraw after typing (frame loop stuck)" shot_differs "$OUT/woke.ppm"
  grim "$OUT/typed.png"

  locked || fail "the session unlocked"
  echo "PASS"
'
