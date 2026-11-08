#!/usr/bin/env bash
# Files PT1: the AppChooser and Access portals served by `andromeda-files --picker`. Fails when:
#   - ChooseApplication shows no window, or the chosen app does not come back as the choice,
#   - Cancel does not answer 1, or the caller's Request.Close leaves the window open or the call unanswered,
#   - AccessDialog Allow does not return the choices, or Deny does not answer 1.
# Writes the picker and client logs to $OUT (default ./artifacts/files-app-chooser).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-app-chooser}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
CALL=$ROOT/tests/e2e/appchooser_impl_call.py
PICKER=dev.andromeda.Files.Picker
HOME_DIR=$RUNTIME/home
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
apps=$HOME_DIR/.local/share/applications
mkdir -p "$apps"
for id in org.test.Viewer org.test.Editor; do
  printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true %%f\nMimeType=text/plain;\n' "${id#org.test.}" \
    > "$apps/$id.desktop"
done

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
env_all=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config"
  XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CURRENT_DESKTOP=andromeda)

run "${env_all[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" --picker > "$OUT/picker.log" 2>&1 &

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
wait_for "the picker is not on the bus" on_bus org.freedesktop.impl.portal.desktop.andromeda.Files /org/freedesktop/portal/desktop

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$PICKER\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$PICKER" -o /dev/andromeda/Files/Picker \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
call() { # name, kind, handle, args...
  local name=$1; shift
  run "${env_all[@]}" python3 "$CALL" "$@" > "$OUT/client-$name.log" 2>&1 &
  wait_for "the dialog window did not map ($name)" mapped
  run "$ANDROMEDA" settle > /dev/null
}
answered() { grep -qE "^RESULT" "$OUT/client-$1.log"; }
result() { wait_for "the client got no answer ($1)" answered "$1"; grep -E "^RESULT" "$OUT/client-$1.log"; }

call choose choose /org/test/request/1 org.test.Viewer org.test.Editor
action chooser.select "[<'org.test.Editor'>]"
action chooser.accept
[[ $(result choose) == "RESULT 0 org.test.Editor" ]] || fail "choose returned: $(result choose)"
wait_for "the chooser stayed open after Open" unmapped
echo "PASS choose"

call cancel choose /org/test/request/2 org.test.Viewer
action chooser.cancel
[[ $(result cancel) == "RESULT 1 " ]] || fail "Cancel returned: $(result cancel)"
wait_for "the chooser stayed open after Cancel" unmapped
echo "PASS cancel"

call close choose /org/test/request/3 org.test.Viewer
DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.freedesktop.impl.portal.desktop.andromeda.Files \
  -o /org/test/request/3 -m org.freedesktop.impl.portal.Request.Close > /dev/null
[[ $(result close) == "RESULT 2 " ]] || fail "Request.Close returned: $(result close)"
wait_for "the chooser stayed open after Request.Close" unmapped
echo "PASS request close"

call grant access /org/test/request/4
action access.grant
[[ $(result grant) == "RESULT 0 remember=true" ]] || fail "Allow returned: $(result grant)"
wait_for "the access dialog stayed open after Allow" unmapped
call deny access /org/test/request/5
action access.deny
[[ $(result deny) == "RESULT 1 " ]] || fail "Deny returned: $(result deny)"
wait_for "the access dialog stayed open after Deny" unmapped
echo "PASS access"

# Files' own Open With: the same sheet inside a Files window launches the picked app with the selected file.
DOCS=$HOME_DIR/docs
mkdir -p "$DOCS"
echo text > "$DOCS/note.txt"
APP=dev.andromeda.Files
run "${env_all[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$DOCS" > "$OUT/files.log" 2>&1 &
files_win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
files_mapped() { local w; w=$(files_win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
wait_for "the Files window did not map" files_mapped
files_action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
selected() { files_action select "[<['note.txt']>]"; grep -q "selected 1 of " "$OUT/files.log"; }
wait_for "Files could not select note.txt" selected
files_action file.open-with
files_action chooser.select "[<'org.test.Editor'>]"
files_action chooser.accept
launched() { grep -q "opened 1 items with org.test.Editor" "$OUT/files.log"; }
wait_for "Open With did not launch the picked app" launched
echo "PASS files open with"
