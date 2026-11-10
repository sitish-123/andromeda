#!/usr/bin/env bash
# Files P3: the FileChooser portal. A GTK 4 app (gtk_file_dialog.py, GDK_DEBUG=portals) opens Gtk.FileDialog;
# xdg-desktop-portal routes it to `andromeda-files --picker` (andromeda-files.portal). Fails when:
#   - the picker window does not map for OpenFile, or the app does not get back the chosen file,
#   - the "*.txt" filter shows note.png, or the picker does not let the app pick note.txt,
#   - SaveFile does not return <folder>/<typed name>, or Cancel does not end the dialog as cancelled.
# Writes the picker, portal and client logs to $OUT (default ./artifacts/files-picker).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-picker}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
PORTAL=${PORTAL:-/usr/lib/xdg-desktop-portal}
CLIENT=$ROOT/tests/e2e/gtk_file_dialog.py
PICKER=dev.andromeda.Files.Picker
HOME_DIR=$RUNTIME/home
DOCS=$HOME_DIR/docs
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$DOCS" "$RUNTIME/portals" "$HOME_DIR/.config/xdg-desktop-portal"
echo text > "$DOCS/note.txt"
printf 'png' > "$DOCS/note.png"
cp "$ROOT/apps/files/data/andromeda-files.portal" "$RUNTIME/portals/"
printf '[preferred]\ndefault=none\norg.freedesktop.impl.portal.FileChooser=andromeda-files\n' \
  > "$HOME_DIR/.config/xdg-desktop-portal/andromeda-portals.conf"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_all=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config"
  XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CURRENT_DESKTOP=andromeda)

run "${env_all[@]}" ANDROMEDA_LOG_LEVEL=debug "$FILES" --picker > "$OUT/picker.log" 2>&1 &
run "${env_all[@]}" XDG_DESKTOP_PORTAL_DIR="$RUNTIME/portals" "$PORTAL" --verbose > "$OUT/portal.log" 2>&1 &

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
wait_for "the picker is not on the bus" on_bus org.freedesktop.impl.portal.desktop.andromeda.Files /org/freedesktop/portal/desktop
wait_for "xdg-desktop-portal is not on the bus" on_bus org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$PICKER\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$PICKER" -o /dev/andromeda/Files/Picker \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
# The listing is async: select again until the picker logs the selection with the expected count.
select_names() { # count, names gvariant
  local before
  for _ in $(seq 40); do
    before=$(wc -l < "$OUT/picker.log")
    action select "[<$2>]"
    sleep 0.05
    grep -q "selected $1 of " < <(tail -n +"$((before + 1))" "$OUT/picker.log") && return 0
  done
  fail "could not select $2 (wanted $1)"
}
client() { # name, mode, args...
  local name=$1; shift
  run "${env_all[@]}" GDK_DEBUG=portals GTK_A11Y=none python3 "$CLIENT" "$@" > "$OUT/client-$name.log" 2>&1 &
  wait_for "the picker window did not map ($name)" mapped
  run "$ANDROMEDA" settle > /dev/null
}
result() { # name
  wait_for "the client got no answer ($1)" grep -qE "^(RESULT|CANCELLED)" "$OUT/client-$1.log"
  grep -E "^(RESULT|CANCELLED)" "$OUT/client-$1.log"
}

client open open "$DOCS"
# note.txt first: a count of 1 proves the listing is in, so the 0 for note.png comes from the filter.
select_names 1 "['note.txt']"
select_names 0 "['note.png']"
select_names 1 "['note.txt']"
action picker.accept
[[ $(result open) == "RESULT $DOCS/note.txt" ]] || fail "OpenFile returned: $(result open)"
wait_for "the picker window stayed open after Open" unmapped
echo "PASS open (txt filter hides note.png)"

client save save "$DOCS" draft.txt
action picker.name "[<'report.txt'>]"
action picker.accept
[[ $(result save) == "RESULT $DOCS/report.txt" ]] || fail "SaveFile returned: $(result save)"
wait_for "the picker window stayed open after Save" unmapped
echo "PASS save"

# Portal choices come back as chosen (a pop keeps its default, a check its value), and a Save with no folder from the
# caller starts where this app saved last.
impl() { # name, method, app id, [folder]
  local name=$1; shift
  run "${env_all[@]}" python3 "$ROOT/tests/e2e/filechooser_impl_call.py" "$@" > "$OUT/client-$name.log" 2>&1 &
  wait_for "the picker window did not map ($name)" mapped
  run "$ANDROMEDA" settle > /dev/null
}
impl choices1 SaveFile org.test.Editor "$DOCS"
action picker.name "[<'one.txt'>]"
action picker.accept
[[ $(result choices1) == "RESULT 0 file://$DOCS/one.txt quality=low,readonly=true" ]] \
  || fail "choices came back as: $(result choices1)"
wait_for "the picker window stayed open after the choices Save" unmapped
impl remembered SaveFile org.test.Editor
action picker.name "[<'two.txt'>]"
action picker.accept
[[ $(result remembered) == "RESULT 0 file://$DOCS/two.txt quality=low,readonly=true" ]] \
  || fail "a Save with no folder did not start in the app's last folder: $(result remembered)"
echo "PASS choices + last folder per app"

client cancel open "$DOCS"
action picker.cancel
[[ $(result cancel) == CANCELLED* ]] || fail "Cancel returned: $(result cancel)"
wait_for "the picker window stayed open after Cancel" unmapped
echo "PASS cancel"
