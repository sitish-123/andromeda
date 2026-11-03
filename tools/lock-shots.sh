#!/usr/bin/env bash
# Screenshots of the Andromeda lock screen in a headless session: idle, typing, caps-lock hint, lock from phone.
# Usage: SCALE=1.5 LAYOUT=split CLOCK=stacked CLOCK_SIZE=xl CARDS=1 WALL=path PHONE=1 tools/lock-shots.sh [out-dir]; PHONE=1 locks the way the phone does;
# CARDS=1 plays the E2E MPRIS player and posts notifications first. Never submits a password (the real faillock count stays).
set -euo pipefail
OUT=${1:-$(pwd)/artifacts/lock-shots}
SCALE=${SCALE:-1}
LAYOUT=${LAYOUT:-centered}
CLOCK=${CLOCK:-classic}
CLOCK_SIZE=${CLOCK_SIZE:-}
WALL=${WALL:-$(pwd)/.agent/mockups/lock-wall.jpg}
source "$(dirname "$0")/../tests/e2e/lib.sh"
rm -rf "$OUT"
boot_headless 1
printf '\n[output.HEADLESS-1]\nscale = %s\n' "$SCALE" >> "$RUNTIME/andromeda.toml"
printf '\n[lockscreen]\ntheme = "andromeda"\nlayout = "%s"\nclock_style = "%s"\nclock_size = "%s"\nwallpaper = "%s"\nowner_message = "If found, call +977 98XX XXX XXX"\n' "$LAYOUT" "$CLOCK" "$CLOCK_SIZE" "$WALL" \
  >> "$RUNTIME/home/.config/noctalia/config.toml"

KEYS=${KEYS:-$ROOT/compositor/build-debug/tests/pointer-client}
with_noctalia '
  KEYS='"$KEYS"'
  PHONE='"${PHONE:-0}"'
  CARDS='"${CARDS:-0}"'
  PLAYER='"$ROOT/tests/e2e/mpris_test_player.py"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  wtype -s 3600000 &
  sleep 0.3
  if [[ $CARDS == 1 ]]; then
    python3 "$PLAYER" /dev/null &
    notify-send -a Mail "Invoice" "secret body"
    notify-send -a Mail "Lunch" "secret body"
    notify-send -a WhatsApp "Maya" "secret body"
    sleep 1 # real time: the player registers and the notifications land in history
  fi
  if [[ $PHONE == 1 ]]; then
    busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.andromeda.Shell1 /org/andromeda/Shell1 \
      org.andromeda.Shell1 LockFromPhone || fail "LockFromPhone failed"
  else
    [[ $(msg session lock) == ok ]] || fail "lock refused"
  fi
  wait_for "did not lock" locked
  sleep 2 # real time: the enter transition and the wallpaper settle
  grim "$OUT/idle.png"
  wtype abcdef
  sleep 0.5
  grim "$OUT/typing.png"
  "$KEYS" 1280 720 lock caps > /dev/null
  sleep 0.5
  grim "$OUT/caps.png"
'
echo "shots in $OUT"
