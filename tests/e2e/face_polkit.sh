#!/usr/bin/env bash
# polkit sheet with face (docs/face.md P1, P2, P6) on the real stack: the machine's polkit, /etc/pam.d/polkit-1 with
# pam_gaze.so, the running gazed, and the enrolled user in front of the camera. The shell under test registers its
# agent for one waiting process only (NOCTALIA_POLKIT_TEST_PROCESS), so the user's own agent keeps the session. That
# process then runs `pkexec true`: the sheet must show the face prompt from pam_gaze's markers, then Confirm after the
# match, and Enter must authorize. Needs FACE_LIVE=1 and a person, so `just e2e` skips it. Writes look.png,
# confirm.png, noctalia.log, and pkexec.txt to $OUT (default ./artifacts/face-polkit).
set -euo pipefail
[[ ${FACE_LIVE:-} == 1 ]] || { echo "SKIP: face_polkit.sh needs FACE_LIVE=1 and the enrolled user at the camera"; exit 0; }
OUT=${OUT:-$(pwd)/artifacts/face-polkit}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
fail() { echo "FAIL: $*" >&2; exit 1; }
logged() { grep -q "$1" "$OUT/noctalia.log"; }
wait_for() { local what=$1; shift; for _ in $(seq 150); do "$@" && return; sleep 0.1; done; fail "$what"; }

# pkexec asks on behalf of its parent, this script, so the agent serves this process.
subject=$$
# The shell under test tells gazed to send markers to polkit-1; take that back so the user's own agent does not get them.
trap 'busctl --system call com.gundulabs.Gaze /com/gundulabs/Gaze com.gundulabs.Gaze RemovePamInternal s polkit-1 \
  > /dev/null 2>&1 || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT

printf '[shell]\npolkit_agent = true\n' >> "$RUNTIME/home/.config/noctalia/config.toml"
# A private session bus keeps this shell off the user's notification and MPRIS names; the system bus stays real.
run env GIO_USE_VFS=local NOCTALIA_POLKIT_TEST_PROCESS="$subject" dbus-run-session --config-file="$RUNTIME/bus.conf" -- "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
wait_for "the agent did not register" logged "polkit authentication agent active"
wait_for "gazed was not told about polkit-1" logged "gazed sends face prompts to this agent"
run wtype -s 3600000 &

pkexec --disable-internal-agent /usr/bin/true > "$OUT/pkexec.txt" 2>&1 &
pk=$!
wait_for "no authentication request" logged "authentication request for"
wait_for "no face prompt on the sheet" logged "face prompt GAZE_MSG_LOOK"
sleep 0.5
run grim "$OUT/look.png"
wait_for "the face never matched" logged "face prompt GAZE_REQUIRE_CONFIRMATION"
sleep 0.3
run grim "$OUT/confirm.png"
run wtype -k Return
wait "$pk" && rc=0 || rc=$?
echo "exit=$rc" >> "$OUT/pkexec.txt"
[[ $rc == 0 ]] || fail "pkexec was not authorized (exit $rc)"
logged "authorized as" || fail "the agent did not report the authorization"
echo "PASS: pkexec authorized by face and Enter through the shell's sheet; artifacts: $OUT"
