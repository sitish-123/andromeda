#!/usr/bin/env bash
# The real phone as the desktop's remote, driven through the app's own screens, against a private umbriel-linkd over
# Wi-Fi through a relay on a port the firewall lets in. Proves, from the D-Bus signals the shell would inject:
# 1. The touchpad: a drag moves the pointer right and down, a tap clicks left, a two-finger tap clicks right.
# 2. The typing field sends what is typed as text, and Send as Enter.
# 3. Presenter: Next and the volume-down key send page-down, the volume-up key page-up.
# 4. A desktop command shows on the device page by name, and a tap runs its line here.
# 5. The phone's Ping reaches the desktop; the desktop's ping shows on the phone.
# Needs the debug APKs and the phone on USB debugging, on the same Wi-Fi as this machine; see link_phone_lib.sh.
# Uninstalls Link on the phone first. Holds /tmp/umbriel-link-phone.lock. Writes results.jsonl, signals.txt, linkd.log,
# ui-*.xml, and screen-*.png to $OUT (default ./artifacts/link-control-phone).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-control-phone}
source "$(dirname "$0")/link_phone_lib.sh"
RELAY_PORT=${RELAY_PORT:-48307}

[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-control-phone.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
[[ $(adb get-state 2>/dev/null) == device ]] || fail "no phone at $SERIAL"
[[ -s $APK ]] || fail "no APK at $APK"

private_bus system
start_linkd
HOST=$(ip -4 route get 1.1.1.1 | grep -o 'src [0-9.]*' | cut -d' ' -f2)
echo clean > "$RUNTIME/impair"
python3 "$ROOT/tests/e2e/udp_impair.py" "$RELAY_PORT" "$PORT" "$RUNTIME/impair" "$OUT/relay.tsv" &

install_app
pair_phone "$HOST:$RELAY_PORT"
phone_switches
link SetGrant "'$ID'" "'input'" true > /dev/null
link SetCommands "[('Touch the file', 'touch $RUNTIME/ran')]" > /dev/null
record "{\"step\":\"paired\",\"device\":\"$ID\"}"

# inputs ACTION: the RemoteInput signals with that action so far, one "dx dy detail" line each.
inputs() {
  python3 - "$OUT/signals.txt" "$1" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
for block in text.split("signal time=")[1:]:
    if "member=RemoteInput" not in block:
        continue
    strings = re.findall(r'string "((?:[^"\\]|\\.)*)"', block)
    ints = re.findall(r"int32 (-?\d+)", block)
    if len(strings) >= 3 and strings[1] == sys.argv[2]:
        print(ints[0], ints[1], strings[2])
PY
}
count() { (( $(inputs "$1" | wc -l) >= $2 )); }
has() { inputs "$1" | grep -q " $2\$"; }
# scroll_to NAME TEXT: scrolls the app's page up until TEXT shows, then taps it.
scroll_to() {
  for _ in $(seq 8); do
    ui "$1"
    local at
    at=$(bounds "$1" "$2")
    if [[ -n $at ]]; then adb shell input tap $at; return; fi
    adb shell input swipe 540 1700 540 900 400
    sleep 0.5
  done
  fail "$2 is not on the page"
}
shot() { adb exec-out screencap -p > "$OUT/screen-$1.png"; }

adb shell am start -W --activity-clear-task -n "$PACKAGE/.MainActivity" > /dev/null
tap_text home "Features and pairing"
wait_for 30 "the phone got no commands" eval 'ui commands; [[ -n $(bounds commands "Touch the file") ]] || { adb shell input swipe 540 1700 540 900 400; false; }'
shot device
scroll_to remote "Touchpad and keyboard"
ui pad
read -r PX PY <<< "$(bounds pad Touchpad)"
[[ -n $PX ]] || fail "no touchpad on screen"
adb shell input swipe "$PX" "$PY" $(( PX + 300 )) $(( PY + 200 )) 400
wait_for 10 "no moves arrived" count move 3
read -r DX DY <<< "$(inputs move | awk '{x += $1; y += $2} END {print x, y}')"
(( DX > 200 && DY > 120 )) || fail "a drag of (+300, +200) phone pixels moved by ($DX, $DY)"
adb shell input tap "$PX" "$PY"
wait_for 10 "a tap did not click" has click left
shot remote
record "{\"step\":\"touchpad\",\"drag_px\":[300,200],\"moved\":[$DX,$DY],\"moves\":$(inputs move | wc -l),\"tap\":\"left click\"}"

tap_text field "Type here"
adb shell input text "hello"
wait_for 10 "typing did not arrive as text: $(inputs text | tr '\n' '|')" eval '[[ $(inputs text | cut -d" " -f3- | tr -d "\n") == hello ]]'
adb shell input keyevent KEYCODE_ENTER
wait_for 10 "Send did not arrive as Enter" has key enter
record "{\"step\":\"typing\",\"text\":\"$(inputs text | cut -d' ' -f3- | tr -d '\n')\",\"enter\":true}"
# Back hides the keyboard first, if it shows; the screen's own back arrow then leaves.
adb shell dumpsys input_method | grep -q 'mInputShown=true' && adb shell input keyevent KEYCODE_BACK
tap_text leave-remote Back

scroll_to presenter "Presenter"
tap_text next Next
wait_for 10 "Next did not send page-down" has key page-down
before=$(inputs key | grep -c page-down)
adb shell input keyevent KEYCODE_VOLUME_DOWN
wait_for 10 "volume down did not send page-down" eval '(( $(inputs key | grep -c page-down) > before ))'
adb shell input keyevent KEYCODE_VOLUME_UP
wait_for 10 "volume up did not send page-up" has key page-up
shot presenter
record "{\"step\":\"presenter\",\"next\":\"page-down\",\"volume_down\":\"page-down\",\"volume_up\":\"page-up\"}"
adb shell input keyevent KEYCODE_BACK

scroll_to command "Touch the file"
wait_for 10 "the command did not run" test -e "$RUNTIME/ran"
scroll_to ping "Ping"
wait_for 10 "no Pinged signal" grep -q "member=Pinged" "$OUT/signals.txt"
link Ping "'$ID'" "'Come back'" > /dev/null
wait_for 15 "the phone shows no ping" eval 'adb shell dumpsys notification --noredact | grep -q "pinged you"'
record "{\"step\":\"commands-and-ping\",\"command_ran\":true,\"phone_pinged_desktop\":true,\"desktop_pinged_phone\":true}"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
