#!/usr/bin/env bash
# Link on another desktop: andromeda-linkd, andromeda-link-session, the Link app, and a present headless phone, on a
# private bus with a private PipeWire. COMPOSITOR picks the session: sway (headless Sway: data-control, wlr virtual
# pointer and keyboard), kwin (nested KWin: data-control, libei through the RemoteDesktop portal, pre-authorized so no
# dialog shows), x11 (Xvfb: the CLIPBOARD selection, XTest), or mutter (a nested GNOME Shell window: the Clipboard
# portal and libei; its RemoteDesktop dialog needs one click on Share, so it is not headless; skipped with SKIP when
# gnome-shell or xdg-desktop-portal-gnome is missing). Proves, in each: the daemon's Link1 matches
# api/org.andromeda.Link1.xml; the phone pairs with the QR code the app shows; link-session picks the expected
# backends; a phone notification shows
# on the session's notification server with its buttons, and a button runs its action on the phone; a desktop copy
# reaches the phone, and a phone clip pastes on the desktop byte for byte; with the input grant, a move arrives as the
# same relative motion, a left click as button 1, and text as an entry's text (Unicode, except on KWin, whose EIS
# server has no ei_text); with the system grant, the phone sets the volume and mute and sees a desktop-side change;
# the phone and SessionCapabilities get what link-session can do, and nothing once it exits. Writes app-pairing.png,
# app-light.png, app-dark.png, steps.txt, session.log, app.log, notifier.jsonl, window.jsonl, hold.jsonl, linkd.log,
# and the compositor's log to $OUT (default ./artifacts/link-portable-$COMPOSITOR).
set -euo pipefail
COMPOSITOR=${COMPOSITOR:-sway}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-portable-$COMPOSITOR}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
E2E=$ROOT/tests/e2e

if [[ ${1:-} != inner ]]; then
  case $COMPOSITOR in
    sway | kwin | x11) ;;
    mutter)
      for need in gnome-shell /usr/lib/xdg-desktop-portal-gnome; do
        command -v "$need" > /dev/null || { echo "SKIP: COMPOSITOR=mutter needs $need"; exit 0; }
      done
      ;;
    *) echo "COMPOSITOR is sway, kwin, x11, or mutter, not $COMPOSITOR" >&2; exit 2 ;;
  esac
  rm -rf "$OUT"
  mkdir -p "$OUT"
  RUNTIME=$(mktemp -d /tmp/lss.XXXX)
  trap 'rm -rf "$RUNTIME"' EXIT
  mkdir -p "$RUNTIME/home" "$RUNTIME/data" "$RUNTIME/state" "$RUNTIME/config"
  # No service directories, so the bus never activates the installed andromeda-linkd or a desktop's own services.
  cat > "$RUNTIME/bus.conf" << 'CONF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
  HOST_WAYLAND=${WAYLAND_DISPLAY:+${XDG_RUNTIME_DIR:-/run/user/$UID}/$WAYLAND_DISPLAY}
  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS COMPOSITOR="$COMPOSITOR" OUT="$OUT" RUNTIME="$RUNTIME" \
    HOST_WAYLAND="$HOST_WAYLAND" \
    HOME="$RUNTIME/home" XDG_RUNTIME_DIR="$RUNTIME" XDG_DATA_HOME="$RUNTIME/data" XDG_STATE_HOME="$RUNTIME/state" \
    XDG_CONFIG_HOME="$RUNTIME/config" NO_AT_BRIDGE=1 GTK_A11Y=none \
    dbus-run-session --config-file="$RUNTIME/bus.conf" -- "$0" inner
  cat "$OUT/steps.txt"
  echo "PASS; artifacts: $OUT"
  exit 0
fi

trap 'kill $(jobs -p) 2> /dev/null || true' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
wait_for() {
  local what=$1; shift
  for _ in $(seq "${TRIES:-150}"); do "$@" && return; sleep 0.1; done
  fail "$what"
}
link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
property() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 "$1"; }
devices() { property Devices; }
say() { printf "%s\n" "$*" >&3; }
notifier() { printf "%s\n" "$*" >&4; }
phone_saw() { grep -q "$1" "$OUT/hold.jsonl"; }
seen() { cat "$OUT/window.jsonl" 2> /dev/null | grep -c "\"event\": \"$1\"" || true; }
last() { grep "\"event\": \"$1\"" "$OUT/window.jsonl" | tail -1; }
field() { python3 -c "import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])" "$1" "$2" 2> /dev/null; }
count() { (( $(seen "$1") >= $2 )); }
at() { [[ $(field "$(last motion)" x) == "$1" && $(field "$(last motion)" y) == "$2" ]]; }
typed() { [[ "$(field "$(last text)" text)" == "$1" ]]; }

# A private PipeWire with one null sink; WirePlumber's device monitors are off, so no real card is touched. It
# starts first: GNOME Shell's devkit window shows its screen through PipeWire and exits without it.
mkdir -p "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d"
cat > "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d/90-e2e.conf" << 'CONF'
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
CONF
pipewire > "$OUT/pipewire.log" 2>&1 &
wait_for "pipewire did not start" test -S "$RUNTIME/pipewire-0"
wireplumber > "$OUT/wireplumber.log" 2>&1 &
pipewire-pulse > "$OUT/pipewire-pulse.log" 2>&1 &
wait_for "pipewire-pulse did not start" eval "pactl info > /dev/null 2>&1"
pactl load-module module-null-sink sink_name=e2e > /dev/null
wait_for "no default sink" eval "wpctl get-volume @DEFAULT_AUDIO_SINK@ > /dev/null 2>&1"

case $COMPOSITOR in
  sway)
    printf 'output * resolution 1280x720\ndefault_border none\n' > "$RUNTIME/sway.conf"
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c "$RUNTIME/sway.conf" > "$OUT/sway.log" 2>&1 &
    wait_for "sway did not start" eval "ls $RUNTIME/wayland-* > /dev/null 2>&1"
    export WAYLAND_DISPLAY=$(basename "$(ls "$RUNTIME"/wayland-* | grep -v lock | head -1)")
    CLIPBOARD=ext-data-control-v1 INPUT=wlr-virtual-pointer GDK=wayland
    copy() { printf %s "$1" | wl-copy; }
    paste() { wl-paste -n -t "$1"; }
    screenshot() { grim "$1"; }
    ;;
  kwin)
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland QT_QPA_PLATFORM=wayland
    KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 \
      kwin_wayland --virtual --no-lockscreen --width 1280 --height 720 --socket wayland-k > "$OUT/kwin.log" 2>&1 &
    wait_for "kwin did not start" test -S "$RUNTIME/wayland-k"
    export WAYLAND_DISPLAY=wayland-k
    /usr/lib/xdg-permission-store > "$OUT/permission-store.log" 2>&1 &
    /usr/lib/xdg-desktop-portal-kde > "$OUT/portal-kde.log" 2>&1 &
    sleep 1 # real time: the backend owns its name before the frontend looks for it
    /usr/lib/xdg-desktop-portal -r > "$OUT/portal.log" 2>&1 &
    wait_for "the RemoteDesktop portal did not start" eval \
      "busctl --user get-property org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.RemoteDesktop version > /dev/null 2>&1"
    # The KDE portal skips its dialog for an app pre-authorized here; a host process has the empty app id.
    flatpak permission-set kde-authorized remote-desktop "" yes
    CLIPBOARD=ext-data-control-v1 INPUT="libei on the RemoteDesktop portal" GDK=wayland
    copy() { printf %s "$1" | wl-copy; }
    paste() { wl-paste -n -t "$1"; }
    screenshot() { python3 "$E2E/kwin_screenshot.py" "$1"; }
    ;;
  x11)
    Xvfb :77 -screen 0 1280x720x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
    export DISPLAY=:77
    wait_for "Xvfb did not start" xdpyinfo -display :77 > /dev/null 2>&1
    CLIPBOARD="the X11 CLIPBOARD selection" INPUT=XTest GDK=x11
    copy() { printf %s "$1" | xclip -selection clipboard -i; }
    paste() { xclip -selection clipboard -o -t "$1"; }
    screenshot() { import -window root "$1"; }
    ;;
  mutter)
    # A GNOME Shell window on the host's display; --unsafe-mode lets the test call its Screenshot API.
    export XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_TYPE=wayland
    [[ -n ${HOST_WAYLAND:-} ]] || fail "COMPOSITOR=mutter runs in a Wayland session, for its window"
    WAYLAND_DISPLAY=$HOST_WAYLAND gnome-shell --devkit --wayland --unsafe-mode --wayland-display wayland-g \
      > "$OUT/gnome-shell.log" 2>&1 &
    wait_for "gnome-shell did not start" test -S "$RUNTIME/wayland-g"
    export WAYLAND_DISPLAY=wayland-g
    /usr/lib/xdg-permission-store > "$OUT/permission-store.log" 2>&1 &
    /usr/lib/xdg-desktop-portal-gnome > "$OUT/portal-gnome.log" 2>&1 &
    sleep 1 # real time: the backend owns its name before the frontend looks for it
    /usr/lib/xdg-desktop-portal -r > "$OUT/portal.log" 2>&1 &
    wait_for "the RemoteDesktop portal did not start" eval \
      "busctl --user get-property org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.RemoteDesktop version > /dev/null 2>&1"
    echo "COMPOSITOR=mutter: click Share in the GNOME Shell window when the remote desktop dialog shows" >&2
    CLIPBOARD="the Clipboard portal" INPUT="libei on the RemoteDesktop portal" GDK=wayland
    copy() { printf %s "$1" | wl-copy; }
    paste() { wl-paste -n -t "$1"; }
    screenshot() {
      gdbus call --session -d org.gnome.Shell.Screenshot -o /org/gnome/Shell/Screenshot \
        -m org.gnome.Shell.Screenshot.Screenshot false false "$1" > /dev/null
    }
    ;;
esac
shot() { sleep 1.5; screenshot "$OUT/app-$1.png"; } # real time: the window repaints

volume() { wpctl get-volume @DEFAULT_AUDIO_SINK@; }

mkfifo "$RUNTIME/notifier.in"
exec 4<> "$RUNTIME/notifier.in"
python3 "$E2E/link_notifier.py" "$OUT/notifier.jsonl" < "$RUNTIME/notifier.in" 2> "$OUT/notifier.log" &
wait_for "the notifier did not start" grep -qs ready "$OUT/notifier.jsonl"

"$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
wait_for "andromeda-linkd did not start" eval "devices > /dev/null 2>&1"
gdbus introspect --session -d org.andromeda.Link1 -o /org/andromeda/Link1 --xml > "$OUT/link1-live.xml"
python3 "$E2E/link_api_check.py" "$ROOT/link/api/org.andromeda.Link1.xml" "$OUT/link1-live.xml" > "$OUT/api-check.txt" \
  || fail "the daemon differs from api/org.andromeda.Link1.xml: $(cat "$OUT/api-check.txt")"
step "api: the daemon's Link1 matches api/org.andromeda.Link1.xml"
PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['port'])" "$RUNTIME/state/andromeda-link/devices.json")
app() {
  GDK_BACKEND=$GDK ANDROMEDA_LINK_DATA="$ROOT/link/crates/link-app/data" ADW_DEBUG_COLOR_SCHEME=$1 \
    "$BIN/andromeda-link" >> "$OUT/app.log" 2>&1 &
  APP=$!
  wait_for "the app did not start" eval \
    "gdbus call --session -d org.andromeda.Link -o /org/andromeda/Link/window/1 -m org.gtk.Actions.List 2> /dev/null | grep -q pair"
}
app prefer-light
gdbus call --session -d org.andromeda.Link -o /org/andromeda/Link/window/1 -m org.gtk.Actions.Activate pair "[]" "{}" \
  > /dev/null || fail "no pair action"
wait_for "Pair a phone opened no pairing window" eval "[[ \$(property Pairing) == *true* ]]"
shot pairing
# The phone scans the QR code on the screen, as the camera would.
URI=$(zbarimg -q --raw "$OUT/app-pairing.png") || fail "no QR code on the screen"
"$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --uri "$URI" --addr "127.0.0.1:$PORT" \
  >> "$OUT/phone.log" 2>&1 || fail "pairing with the QR code the app showed"
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
[[ -n $ID ]] || fail "the phone is not listed: $(devices)"
step "pairing: the phone paired with the QR code the app showed"

"$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
SESSION=$!
# On Mutter link-session waits for a person to click Share on the portal's dialog: 2 minutes.
TRIES=$([[ $COMPOSITOR == mutter ]] && echo 1200 || echo 150) \
  wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
grep -q "clipboard through $CLIPBOARD" "$OUT/session.log" || fail "clipboard backend: $(grep clipboard "$OUT/session.log")"
grep -q "input through $INPUT" "$OUT/session.log" || fail "input backend: $(grep input "$OUT/session.log")"
step "$COMPOSITOR: clipboard through $CLIPBOARD, input through $INPUT"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
"$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold < "$RUNTIME/phone.in" \
  > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
wait_for "the phone never connected" eval "devices | grep -q \"'$ID', 'E2E Phone', true\""
CAN='"can":\["notifications","clipboard","input","volume"\]'
wait_for "the phone did not get the capabilities: $(grep capabilities "$OUT/hold.jsonl")" phone_saw "$CAN"
property SessionCapabilities | grep -q "\['notifications', 'clipboard', 'input', 'volume'\]" \
  || fail "SessionCapabilities: $(property SessionCapabilities)"
step "capabilities: the phone and SessionCapabilities list notifications, clipboard, input, volume"

say 'notify {"id":"chat-1","app":"Messages","title":"Ann Lee","text":"Are you coming?","actions":[{"id":"1","label":"Mark as read","reply":false}]}'
wait_for "the notification did not reach the notifier" grep -q '"summary": "Ann Lee"' "$OUT/notifier.jsonl"
grep '"summary": "Ann Lee"' "$OUT/notifier.jsonl" | grep -q '"phone:1", "Mark as read"' || fail "no button: $(tail -1 "$OUT/notifier.jsonl")"
SHOWN=$(field "$(grep '"summary": "Ann Lee"' "$OUT/notifier.jsonl" | tail -1)" id)
notifier "invoke $SHOWN phone:1"
wait_for "the phone did not get the action" phone_saw '"action":"1".*"event":"notification-action".*"id":"chat-1"'
step "notification: shown with Mark as read; the button ran action 1 on the phone"

copy "copied on $COMPOSITOR"
wait_for "the phone did not get the desktop copy" phone_saw "\"text\":\"copied on $COMPOSITOR\""
printf "copied on the phone" > "$RUNTIME/phone.txt"
say "clip $RUNTIME/phone.txt text/plain;charset=utf-8 text/plain"
wait_for "the phone clip did not paste" eval "[[ \$(paste 'text/plain;charset=utf-8' 2> /dev/null) == 'copied on the phone' ]]"
step "clipboard: the desktop copy reached the phone; the phone clip pasted on the desktop"

link SetGrant "$ID" input true > /dev/null
GDK_BACKEND=$GDK python3 "$E2E/link_input_window.py" "$OUT/window.jsonl" 2> "$OUT/window.log" &
INPUT_WINDOW=$!
wait_for "the input window did not start" count ready 1
sleep 1 # real time: the window maps and takes the keyboard focus
if [[ $COMPOSITOR == x11 ]]; then
  # Xvfb has no window manager to honour fullscreen, so the window is sized here and the pointer put on it.
  WINDOW=$(xdotool search --sync --name link-input | head -1)
  xdotool windowsize "$WINDOW" 1280 720 windowmove "$WINDOW" 0 0 mousemove 200 300
  sleep 0.5 # real time: the window takes its new size
fi
say 'input {"action":"move","dx":5,"dy":5}'
wait_for "the first move did not reach the window" count motion 1
sleep 0.3 # real time: the enter and first motion settle
before=$(last motion)
x0=$(field "$before" x); y0=$(field "$before" y)
say 'input {"action":"move","dx":40,"dy":-25}'
wait_for "the move did not arrive as (+40, -25) from ($x0, $y0): $(last motion)" at $(( x0 + 40 )) $(( y0 - 25 ))
say 'input {"action":"move","dx":0,"dy":300}'
say 'input {"action":"click","button":"left"}'
wait_for "no left click" grep -q '"release", "button": 1' "$OUT/window.jsonl"
# KWin's and Mutter's EIS servers have no ei_text, so libei types only what the keymap has.
if [[ $COMPOSITOR == kwin || $COMPOSITOR == mutter ]]; then TEXT="hello world"; else TEXT="héllo wörld"; fi
say "input {\"action\":\"text\",\"text\":\"$TEXT\"}"
wait_for "the text did not arrive: $(last text)" typed "$TEXT"
step "input: (+40, -25) moved the same, left click as button 1, \"$TEXT\" typed into the entry"

wait_for "the phone did not get the volume: $(grep desktop-status "$OUT/hold.jsonl")" \
  phone_saw '"event":"desktop-status","muted":false,"volume":100'
say 'system {"command":"set-volume","value":30}'
wait_for "the daemon did not refuse set-volume without the system grant" \
  grep -q "dropped a system-command its grant does not allow" "$OUT/linkd.log"
[[ $(volume) == "Volume: 1.00" ]] || fail "set-volume without the grant changed the volume: $(volume)"
link SetGrant "$ID" system true > /dev/null
say 'system {"command":"set-volume","value":30}'
wait_for "set-volume 30 did not reach the sink: $(volume)" eval '[[ $(volume) == "Volume: 0.30" ]]'
say 'system {"command":"set-mute","value":1}'
wait_for "set-mute did not reach the sink: $(volume)" eval '[[ $(volume) == "Volume: 0.30 [MUTED]" ]]'
wait_for "the phone did not see 30%, muted" phone_saw '"event":"desktop-status","muted":true,"volume":30'
wpctl set-volume @DEFAULT_AUDIO_SINK@ 55%
wait_for "the phone did not see a desktop-side change to 55%" phone_saw '"event":"desktop-status","muted":true,"volume":55'
step "volume: the phone saw 100%; without the grant set-volume was refused; with it, 30% and mute reached the sink; a desktop change to 55% reached the phone"

kill "$INPUT_WINDOW"
shot light
kill "$APP"
app prefer-dark
shot dark
step "app: app-light.png with the connected phone, app-dark.png after a restart"

SEEN=$(wc -l < "$OUT/hold.jsonl")
kill "$SESSION"
wait_for "the phone was not told the session left" eval "tail -n +$(( SEEN + 1 )) \"\$OUT/hold.jsonl\" | grep -q '\"can\":\[\]'"
property SessionCapabilities | grep -q "@as \[\]" || fail "SessionCapabilities after exit: $(property SessionCapabilities)"
step "capabilities: when link-session leaves the bus, the phone and SessionCapabilities list none"
