#!/usr/bin/env bash
# PT0: apps of every toolkit open our picker through the FileChooser portal, with the variables the session sets
# (compositor/src/server/server.cpp: GTK_USE_PORTAL=1, GDK_DEBUG=portals, QT_QPA_PLATFORMTHEME=xdgdesktopportal).
# Fails when zenity (GTK) or a QFileDialog client (Qt 6) shows its own dialog instead of the andromeda-files picker, or when the
# picker's Cancel does not end the app's dialog. Shots of each picker go to $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/portal-toolkits}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
PORTAL=${PORTAL:-/usr/lib/xdg-desktop-portal}
PICKER=dev.andromeda.Files.Picker
HOME_DIR=$RUNTIME/home
boot_headless 1
mkdir -p "$OUT" "$HOME_DIR/docs" "$RUNTIME/portals" "$HOME_DIR/.config/xdg-desktop-portal"
fail() { echo "FAIL: $*"; exit 1; }
echo text > "$HOME_DIR/docs/note.txt"
cp "$ROOT/apps/files/data/andromeda-files.portal" "$RUNTIME/portals/"
printf '[preferred]\ndefault=none\norg.freedesktop.impl.portal.FileChooser=andromeda-files\n' \
  > "$HOME_DIR/.config/xdg-desktop-portal/andromeda-portals.conf"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_all=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config"
  XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CURRENT_DESKTOP=andromeda
  GTK_USE_PORTAL=1 GDK_DEBUG=portals QT_QPA_PLATFORMTHEME=xdgdesktopportal)

run "${env_all[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" --picker > "$OUT/picker.log" 2>&1 &

wait_for() { local what=$1; shift; for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
# NameHasOwner, not introspect: a call to an unowned name D-Bus-activates the installed picker.
owned() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
  -m org.freedesktop.DBus.NameHasOwner "$1" | grep -q true; }
wait_for "the picker is not on the bus" owned org.freedesktop.impl.portal.desktop.andromeda.Files
# Only now: xdg-desktop-portal D-Bus-activates the installed picker when the test one is not on the bus yet.
run "${env_all[@]}" XDG_DESKTOP_PORTAL_DIR="$RUNTIME/portals" "$PORTAL" --verbose > "$OUT/portal.log" 2>&1 &
wait_for "xdg-desktop-portal is not on the bus" on_bus org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$PICKER\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
cancel() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$PICKER" -o /dev/andromeda/Files/Picker \
    -m org.freedesktop.Application.ActivateAction picker.cancel '[]' '{}' > /dev/null
}

# toolkit name, command...: the app's file dialog must be our picker; Cancel ends it with a non-zero exit.
check() {
  local name=$1; shift
  command -v "$1" > /dev/null || { echo "SKIP $name: $1 is not installed"; return 0; }
  run "${env_all[@]}" "$@" > "$OUT/$name.log" 2>&1 &
  local pid=$!
  for _ in $(seq 400); do mapped && break; sleep 0.05; done # 20 s: a cold KDE start is slow
  if ! mapped; then
    echo "windows: $(run "$ANDROMEDA" windows --json | jq -c '[.. | objects | .app_id? // empty]')"
    tail -n 5 "$OUT/$name.log"
    fail "$name did not open the andromeda-files picker"
  fi
  run "$ANDROMEDA" settle > /dev/null
  run grim "$OUT/$name.png"
  cancel
  wait_for "$name: the picker did not close on Cancel" unmapped
  local code=0
  wait "$pid" || code=$?
  [[ $code -ne 0 ]] || fail "$name: Cancel did not end the dialog as cancelled"
  echo "ok $name"
}
check zenity zenity --file-selection --filename="$HOME_DIR/docs/"
# kdialog draws KDE's own dialog whatever the platform theme: a plain QFileDialog client is the Qt check.
QT_CLIENT=$RUNTIME/qt-file-dialog
g++ -std=c++17 -fPIC -o "$QT_CLIENT" "$ROOT/tests/e2e/qt_file_dialog.cpp" $(pkg-config --cflags --libs Qt6Widgets Qt6DBus) \
  || fail "the Qt test client did not build"
check qt "$QT_CLIENT" "$HOME_DIR/docs"
echo "pass portal_toolkits"
