#!/usr/bin/env bash
# Lock-screen face unlock (docs/face.md L1-L11) against mock_gaze.py on the test's private system bus. With no gazed
# the lock screen stays on the password; a key press after gazed appears claims and verifies, and verify-match unlocks.
# No enrolled face never claims; a failed Claim never verifies; an unjudged end waits for a key press instead of
# looping; typed password text is never overwritten by face status; gazed exiting mid-verify drops the claim;
# PrepareForSleep stops the verify and resume restarts it; lockscreen.face = false never claims; three judged misses
# stop face unlock until the next lock. Every unlock ends with Release. No password is ever submitted, so the real
# account's faillock count is untouched. Writes calls-*.txt, noctalia.log, mock.log to $OUT
# (default ./artifacts/face-lock).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/face-lock}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  MOCK='"$MOCK"'
  CONFIG=$HOME/.config/andromeda/shell/config.toml
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze "$@" > /dev/null; }
  test_call() { gaze /com/gundulabs/Gaze dsk.test.Gaze "$@"; }
  sleep_signal() { gaze /org/freedesktop/login1 dsk.test.Gaze Sleep b "$1"; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  called() { grep -c "^$1" "$OUT/calls.txt" 2> /dev/null || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  unlocked() { msg status | grep -q "\"locked\": false"; }
  logged() { grep -q "$1" "$OUT/noctalia.log"; }
  reloaded() { [[ $(grep -c "config changed, reloading" "$OUT/noctalia.log") -ge $1 ]]; }
  start_mock() {
    python3 "$MOCK" "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
    mock=$!
    wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze \
      > /dev/null 2>&1
  }
  lock() { : > "$OUT/calls.txt"; [[ $(msg session lock) == ok ]] || fail "lock refused"; wait_for "did not lock" locked; }
  case_done() { cp "$OUT/calls.txt" "$OUT/calls-$1.txt"; }
  verifying() { [[ $(called VerifyStartFor) -ge ${1:-1} ]]; }
  match_unlocks() {
    test_call Finish sssb verify-match usable usable true
    wait_for "$1: verify-match did not unlock" unlocked
    wait_for "$1: unlock did not release gazed" grep -q "^Release" "$OUT/calls.txt"
  }

  kill %1; wait %1 2> /dev/null || true
  # L1-L11 and L4 cover the automatic mode; R1-R3 turn on face_on_request.
  printf "\n[lockscreen]\nface = true\nface_on_request = false\n" >> "$CONFIG"
  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  # A keyboard must exist before the lock surface maps, or the surface never receives keyboard focus.
  wtype -s 3600000 &
  sleep 0.3

  # L1: no gazed: one ask per lock, a key press after gazed appears does not ask again; a face switch off and on
  # (a new start) arms it. L3: a match unlocks.
  lock
  wait_for "L1: no unavailable log" logged "face unlock unavailable"
  locked || fail "L1: unlocked without gazed"
  start_mock
  wtype -k Escape
  sleep 1
  [[ $(called VerifyStartFor) == 0 ]] || fail "L1: a key press asked gazed again in the same lock"
  sed -i "s/^face = true$/face = false/" "$CONFIG"
  wait_for "L1: config not reloaded" reloaded 1
  sed -i "s/^face = false$/face = true/" "$CONFIG"
  wait_for "L1: config not reloaded" reloaded 2
  wait_for "L1: turning face on again did not arm it" verifying
  grep -q "^VerifyStartFor andromeda-lock" "$OUT/calls.txt" || fail "L1: wrong PAM service"
  match_unlocks L3
  case_done L1-L3

  # L2: nothing enrolled, no claim.
  test_call SetEnrolled b false
  lock
  wait_for "L2: enrolment not checked" grep -q "^HasEnrolledFaces" "$OUT/calls.txt"
  sleep 0.5
  [[ $(called Claim) == 0 ]] || fail "L2: claimed with nothing enrolled"
  test_call SetEnrolled b true
  wtype -k Escape
  sleep 1
  [[ $(called Claim) == 0 ]] || fail "L2: a key press asked gazed again in the same lock"
  sed -i "s/^face = true$/face = false/" "$CONFIG"
  wait_for "L2: config not reloaded" reloaded 3
  sed -i "s/^face = false$/face = true/" "$CONFIG"
  wait_for "L2: config not reloaded" reloaded 4
  wait_for "L2: no verify after enrolling" verifying
  match_unlocks L2
  case_done L2

  # L7: Claim fails, no verify.
  test_call SetClaimFails b true
  lock
  wait_for "L7: no claim attempt" grep -q "^Claim" "$OUT/calls.txt"
  sleep 0.5
  [[ $(called VerifyStartFor) == 0 ]] || fail "L7: verified without a claim"
  test_call SetClaimFails b false
  wtype -k Escape
  wait_for "L7: no verify once the camera was free" verifying
  match_unlocks L7
  case_done L7

  # L5: an unjudged end waits for a key press.
  lock
  wait_for "L5: no verify" verifying
  test_call Finish sssb verify-no-match no-face no-face false
  sleep 1
  [[ $(called VerifyStartFor) == 1 ]] || fail "L5: re-armed without user activity"
  wtype -k Escape
  wait_for "L5: a key press did not re-arm" verifying 2
  match_unlocks L5
  case_done L5

  # L6: typed text keeps the prompt; Escape clears it and status shows again.
  lock
  wait_for "L6: no verify" verifying
  msg log-level-set debug > /dev/null
  sleep 2 # real time: the lock transition (1.5 s) ignores typing until it ends
  wtype abc
  sleep 0.5
  test_call Status s too-dark
  wait_for "L6: face status reached a prompt with typed text" logged "status hidden while typing: Too dark"
  msg log-level-set info > /dev/null
  wtype -k Escape
  match_unlocks L6
  case_done L6

  # L8: gazed exits mid-verify.
  lock
  wait_for "L8: no verify" verifying
  test_call Quit
  wait "$mock" 2> /dev/null || true
  wait_for "L8: the exit was not noticed" logged "gazed left the bus"
  start_mock
  wtype -k Escape
  wait_for "L8: no re-claim after gazed returned" grep -q "^Claim" "$OUT/calls.txt"
  wait_for "L8: no verify after gazed returned" verifying
  match_unlocks L8
  case_done L8

  # L9: suspend stops the verify, resume restarts it.
  lock
  wait_for "L9: no verify" verifying
  sleep_signal true
  wait_for "L9: sleep did not stop the verify" grep -q "^VerifyStop" "$OUT/calls.txt"
  sleep_signal false
  wait_for "L9: resume did not re-arm" verifying 2
  match_unlocks L9
  case_done L9

  # L11: lockscreen.face = false never claims.
  sed -i "s/^face = true$/face = false/" "$CONFIG"
  wait_for "L11: config not reloaded" reloaded 5
  lock
  wtype -k Escape
  sleep 1
  [[ $(called Claim) == 0 && $(called HasEnrolledFaces) == 0 ]] || fail "L11: face unlock ran while disabled"
  case_done L11
  locked || fail "L11: unlocked without a face"

  # R1: on request, face on while locked never starts the camera, even after a key press.
  : > "$OUT/calls.txt"
  sed -i -e "s/^face = false$/face = true/" -e "s/^face_on_request = false$/face_on_request = true/" "$CONFIG"
  wait_for "R1: config not reloaded" reloaded 6
  wtype -k Escape
  sleep 1
  [[ $(called Claim) == 0 && $(called VerifyStartFor) == 0 ]] || fail "R1: face started without a request"
  # R2: Enter on the empty field runs one scan; a miss does not start another.
  wtype -k Return
  wait_for "R2: Enter did not start a scan" verifying
  test_call Finish sssb verify-no-match usable usable true
  sleep 1
  wtype -k Escape
  sleep 0.5
  [[ $(called VerifyStartFor) == 1 ]] || fail "R2: a miss started another scan ($(called VerifyStartFor))"
  # R3: the next Enter scans again, and a match unlocks.
  wtype -k Return
  wait_for "R3: second Enter did not scan" verifying 2
  match_unlocks R3
  case_done R3

  # L4 last: after three judged misses face unlock stays off, so this lock never ends by face.
  sed -i "s/^face_on_request = true$/face_on_request = false/" "$CONFIG"
  wait_for "L4: config not reloaded" reloaded 7
  lock
  wait_for "L4: lock did not arm face" verifying
  for n in 1 2 3; do
    wait_for "L4: miss $n had no verify" verifying "$n"
    test_call Finish sssb verify-no-match usable usable true
  done
  sleep 1
  wtype -k Escape
  sleep 0.5
  [[ $(called VerifyStartFor) == 3 ]] || fail "L4: verified after three judged misses ($(called VerifyStartFor))"
  wait_for "L4: no release after the last miss" grep -q "^Release" "$OUT/calls.txt"
  locked || fail "L4: a miss unlocked"
  case_done L4

'
echo "PASS: face lock L1-L11 against a mock gazed; artifacts: $OUT"
