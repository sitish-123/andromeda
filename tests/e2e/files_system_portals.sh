#!/usr/bin/env bash
# Files PT2: Account, Wallpaper, DynamicLauncher and Notification served by `andromeda-files --picker`. Fails when:
#   - Account Share does not return the user's id and name, or Cancel does not answer 1,
#   - Wallpaper with a preview does not show a window and answer 0 on Set, or without one does not set at once,
#   - PrepareInstall does not return the edited name and the icon,
#   - AddNotification does not reach org.freedesktop.Notifications with the button, a server action does not come
#     back as the portal's ActionInvoked with the app's action name, or RemoveNotification does not close it.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-system-portals}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
CALL=$ROOT/tests/e2e/system_portal_call.py
PICKER=dev.andromeda.Files.Picker
HOME_DIR=$RUNTIME/home
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$HOME_DIR"
PICTURE=$ROOT/shell/assets/andromeda-wallpaper.png
ICON=$HOME_DIR/icon.png
python3 -I - "$ICON" <<'PY2'
import struct, sys, zlib
raw = b"".join(b"\0" + bytes((81, 71, 229)) * 64 for _ in range(64))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY2

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_all=(env DESK_FILES_TEST=1 DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config"
  XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CURRENT_DESKTOP=andromeda)

run "${env_all[@]}" python3 "$ROOT/tests/e2e/fake_notification_server.py" > "$OUT/server.log" 2>&1 &
run "${env_all[@]}" ANDROMEDA_LOG_LEVEL=debug "$FILES" --picker > "$OUT/picker.log" 2>&1 &

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
wait_for "the picker is not on the bus" on_bus org.freedesktop.impl.portal.desktop.andromeda.Files /org/freedesktop/portal/desktop
wait_for "the notification server is not on the bus" on_bus org.freedesktop.Notifications /org/freedesktop/Notifications

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$PICKER\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$PICKER" -o /dev/andromeda/Files/Picker \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
start() { # name, args...
  local name=$1; shift
  run "${env_all[@]}" python3 "$CALL" "$@" > "$OUT/client-$name.log" 2>&1 &
}
shown() { wait_for "the dialog did not map ($1)" mapped; run "$ANDROMEDA" settle > /dev/null; }
answered() { grep -qE "^RESULT" "$OUT/client-$1.log"; }
result() { wait_for "the client got no answer ($1)" answered "$1"; grep -E "^RESULT" "$OUT/client-$1.log"; }

start account account /org/test/request/1
shown account
run grim "$OUT/account.png"
action portal.accept
[[ $(result account) == "RESULT 0 id=$(id -un),image=,name="* ]] || fail "Account returned: $(result account)"
wait_for "the account dialog stayed open" unmapped
start account-cancel account /org/test/request/2
shown account-cancel
action portal.cancel
[[ $(result account-cancel) == "RESULT 1 " ]] || fail "Account Cancel returned: $(result account-cancel)"
wait_for "the account dialog stayed open after Cancel" unmapped
echo "PASS account"

start wallpaper wallpaper-preview /org/test/request/3 "$PICTURE"
shown wallpaper
run grim "$OUT/wallpaper.png"
action portal.accept
[[ $(result wallpaper) == "RESULT 0 " ]] || fail "Wallpaper Set returned: $(result wallpaper)"
grep -q "wallpaper set $PICTURE" "$OUT/picker.log" || fail "the wallpaper was not set"
wait_for "the wallpaper dialog stayed open" unmapped
start direct wallpaper-direct /org/test/request/4 "$PICTURE"
[[ $(result direct) == "RESULT 0 " ]] || fail "Wallpaper without preview returned: $(result direct)"
echo "PASS wallpaper"

start launcher launcher /org/test/request/5 "$ICON"
shown launcher
run grim "$OUT/launcher.png"
action portal.name "[<'Mail'>]"
action portal.accept
[[ $(result launcher) == "RESULT 0 name=Mail,icon=True" ]] || fail "PrepareInstall returned: $(result launcher)"
wait_for "the launcher dialog stayed open" unmapped
echo "PASS launcher"

start notify notify n1
result notify > /dev/null
notified() { grep -q "^NOTIFY 1 .* Build done default,,b0,Retry$" "$OUT/server.log"; }
wait_for "the notification did not reach the server" notified
DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
  -m org.freedesktop.Notifications.Invoke 1 b0 > /dev/null
retried() { grep -q "^PORTAL_ACTION org.test.Caller n1 app.retry$" "$OUT/server.log"; }
wait_for "the button action did not come back to the app" retried
start unnotify unnotify n1
result unnotify > /dev/null
closed() { grep -q "^CLOSE 1$" "$OUT/server.log"; }
wait_for "RemoveNotification did not close it" closed
echo "PASS notification"
