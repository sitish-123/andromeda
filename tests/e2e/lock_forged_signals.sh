#!/usr/bin/env bash
# The lock screen ignores unlock signals that its real senders did not send: a VerifyStatus "verify-match" sent
# directly to the shell by another peer than gazed, and a logind session Unlock sent by another peer than logind.
# The real gazed match still unlocks. Writes calls.txt, noctalia.log, mock.log to $OUT (default ./artifacts/lock-forged).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-forged}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  bus() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" "$@"; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  locked() { msg status | grep -q "\"locked\": true"; }
  unlocked() { msg status | grep -q "\"locked\": false"; }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" bus status com.gundulabs.Gaze
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  wtype -s 3600000 &
  sleep 0.3
  shell=$(bus call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus GetNameOwner s \
    org.freedesktop.Notifications | cut -d"\"" -f2)

  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wtype -k Escape
  wait_for "face verify did not start" grep -q "^VerifyStartFor" "$OUT/calls.txt"

  bus emit --destination="$shell" /com/gundulabs/Gaze com.gundulabs.Gaze VerifyStatus "sa(sddbddb)ss" \
    verify-match 1 default 0.62 0 true 0.9 0 false usable usable
  bus emit --destination="$shell" /org/freedesktop/login1/session/test org.freedesktop.login1.Session Unlock
  sleep 1
  locked || fail "a forged VerifyStatus or Unlock from another peer unlocked the session"

  bus call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze Finish sssb verify-match usable usable true
  wait_for "the real gazed match did not unlock" unlocked
  echo "PASS"
'
