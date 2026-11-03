#!/usr/bin/env bash
# The Ryoku theme still locks: with lockscreen.theme = "ryoku" the session locks, typed keys reach the hidden password
# field (the shell keeps running and stays locked), and the style -> theme migration maps a legacy style = "ryoku".
# Writes noctalia.log and ryoku.png to $OUT (default ./artifacts/ryoku-lock).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/ryoku-lock}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
printf '\n[lockscreen]\nstyle = "ryoku"\n' >> "$RUNTIME/home/.config/noctalia/config.toml"

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wtype abc
  sleep 1
  locked || fail "typing unlocked or killed the Ryoku lock"
  grim "$OUT/ryoku.png"
  grep -q "lockscreen.style" "$OUT/noctalia.log" || fail "the legacy style key raised no migration note"
'
echo "PASS: Ryoku theme locks; artifacts: $OUT"
