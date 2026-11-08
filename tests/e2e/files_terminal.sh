#!/usr/bin/env bash
# Files v2 FB-POWER terminal pane (mockup files-v2-live.html §16): F4 shows the user's shell under the view.
# Fails when:
#   - F4 does not start a shell in the shown folder,
#   - a cd in the shell does not move the view, or a folder change in the view does not cd the shell,
#   - the pane types a cd into a running job (sleep), or Ctrl+C does not reach the job,
#   - F4 again does not hide the pane, or "exit" does not close it.
# SCALE=2 writes the 2x pane shot for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-terminal).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-terminal}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
WORK=$HOME_DIR/work
OTHER=$HOME_DIR/other
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$WORK/sub" "$OTHER"
touch "$WORK/notes.txt" "$WORK/build.sh"
# A known prompt and no user rc files, so the screen check does not depend on the machine.
printf 'PS1="\\w \\$ "\n' > "$HOME_DIR/.bashrc"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DESK_FILES_TEST=1 DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" SHELL=/bin/bash)
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$WORK" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
run "$ANDROMEDA" settle > /dev/null

action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
seen() { [[ $(grep -c -- "$1" "$OUT/files.log") -ge $2 ]]; } # the log holds $1 at least $2 times
wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { grep -q -- "$1" "$OUT/files.log"; }
type_text() { action terminal.type "[<'$1'>]"; }
screen_has() { # the newest screen dump holds $1
  action terminal.dump
  awk '/terminal screen:/{buf=""} {buf=buf"\n"$0} END{print buf}' "$OUT/files.log" | grep -qF -- "$1"
}

action view.terminal
wait_for "F4 did not start the shell in work" logged "terminal started /bin/bash in $WORK"
wait_for "the prompt did not show" screen_has "~/work \$"
run "$ANDROMEDA" settle > /dev/null
sleep 0.3
run grim "$OUT/terminal.png"
echo "PASS F4 shows the shell"

type_text 'cd sub\r'
wait_for "the view did not follow cd sub" logged "location $WORK/sub"
echo "PASS the folder follows cd"

action go "[<'$OTHER'>]"
wait_for "the shell did not follow the view" logged "terminal follows $OTHER"
type_text 'pwd > pwd.txt\r'
wait_for "the shell is not in other: $(cat "$OTHER/pwd.txt" 2> /dev/null)" test "$(cat "$OTHER/pwd.txt" 2> /dev/null)" = "$OTHER"
echo "PASS the shell follows the folder"

type_text 'sleep 30\r'
wait_for "sleep did not start" bash -c 'pgrep -f -x "sleep 30" > /dev/null'
action go "[<'$WORK'>]"
wait_for "the pane typed a cd into a running job" logged "terminal busy, no cd to $WORK"
type_text '\u0003'
wait_for "Ctrl+C did not stop the job" bash -c '! pgrep -f -x "sleep 30" > /dev/null'
echo "PASS no cd into a running job; Ctrl+C reaches it"

action view.terminal
wait_for "F4 did not hide the pane" logged "terminal pane off"
action view.terminal
wait_for "F4 did not show the pane again" seen 'terminal pane on' 2
[[ $(grep -c "terminal started" "$OUT/files.log") -eq 1 ]] || fail "hiding the pane ended the shell"
type_text 'exit\r'
wait_for "exit did not close the pane" logged "terminal shell exited"
echo "PASS hide keeps the shell; exit closes it"
