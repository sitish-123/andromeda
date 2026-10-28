#!/usr/bin/env bash
# andromeda-link-session on another desktop, against a real andromeda-linkd and a present headless phone, on a private
# bus. COMPOSITOR picks the session: sway (headless Sway: data-control, wlr virtual pointer and keyboard), kwin (nested
# KWin: data-control, libei through the RemoteDesktop portal, pre-authorized so no dialog shows), or x11 (Xvfb: the
# CLIPBOARD selection, XTest). Proves, in each: link-session picks the expected backends; a phone notification shows
# on the session's notification server with its buttons, and a button runs its action on the phone; a desktop copy
# reaches the phone, and a phone clip pastes on the desktop byte for byte; with the input grant, a move arrives as the
# same relative motion, a left click as button 1, and text as an entry's text (Unicode, except on KWin, whose EIS
# server has no ei_text). Writes steps.txt, session.log,
# notifier.jsonl, window.jsonl, hold.jsonl, linkd.log, and the compositor's log to $OUT
# (default ./artifacts/link-session-$COMPOSITOR).
set -euo pipefail
COMPOSITOR=${COMPOSITOR:-sway}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-session-$COMPOSITOR}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
E2E=$ROOT/tests/e2e

if [[ ${1:-} != inner ]]; then
  case $COMPOSITOR in
    sway | kwin | x11) ;;
    *) echo "COMPOSITOR is sway, kwin, or x11, not $COMPOSITOR" >&2; exit 2 ;;
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
  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS COMPOSITOR="$COMPOSITOR" OUT="$OUT" RUNTIME="$RUNTIME" \
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
  for _ in $(seq 150); do "$@" && return; sleep 0.1; done
  fail "$what"
}
link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 Devices; }
say() { printf "%s\n" "$*" >&3; }
notifier() { printf "%s\n" "$*" >&4; }
phone_saw() { grep -q "$1" "$OUT/hold.jsonl"; }
seen() { cat "$OUT/window.jsonl" 2> /dev/null | grep -c "\"event\": \"$1\"" || true; }
last() { grep "\"event\": \"$1\"" "$OUT/window.jsonl" | tail -1; }
field() { python3 -c "import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])" "$1" "$2" 2> /dev/null; }
count() { (( $(seen "$1") >= $2 )); }
at() { [[ $(field "$(last motion)" x) == "$1" && $(field "$(last motion)" y) == "$2" ]]; }
typed() { [[ "$(field "$(last text)" text)" == "$1" ]]; }

case $COMPOSITOR in
  sway)
    printf 'output * resolution 1280x720\ndefault_border none\n' > "$RUNTIME/sway.conf"
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c "$RUNTIME/sway.conf" > "$OUT/sway.log" 2>&1 &
    wait_for "sway did not start" eval "ls $RUNTIME/wayland-* > /dev/null 2>&1"
    export WAYLAND_DISPLAY=$(basename "$(ls "$RUNTIME"/wayland-* | grep -v lock | head -1)")
    CLIPBOARD=ext-data-control-v1 INPUT=wlr-virtual-pointer GDK=wayland
    copy() { printf %s "$1" | wl-copy; }
    paste() { wl-paste -n -t "$1"; }
    ;;
  kwin)
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland QT_QPA_PLATFORM=wayland
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
    ;;
  x11)
    Xvfb :77 -screen 0 1280x720x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
    export DISPLAY=:77
    wait_for "Xvfb did not start" xdpyinfo -display :77 > /dev/null 2>&1
    CLIPBOARD="the X11 CLIPBOARD selection" INPUT=XTest GDK=x11
    copy() { printf %s "$1" | xclip -selection clipboard -i; }
    paste() { xclip -selection clipboard -o -t "$1"; }
    ;;
esac

mkfifo "$RUNTIME/notifier.in"
exec 4<> "$RUNTIME/notifier.in"
python3 "$E2E/link_notifier.py" "$OUT/notifier.jsonl" < "$RUNTIME/notifier.in" 2> "$OUT/notifier.log" &
wait_for "the notifier did not start" grep -qs ready "$OUT/notifier.jsonl"

"$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
wait_for "andromeda-linkd did not start" eval "devices > /dev/null 2>&1"
PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['port'])" "$RUNTIME/state/andromeda-link/devices.json")
CODE=$(link StartPairing | cut -d"'" -f2)
"$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
  >> "$OUT/phone.log" 2>&1 || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
[[ -n $ID ]] || fail "the phone is not listed: $(devices)"

"$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
grep -q "clipboard through $CLIPBOARD" "$OUT/session.log" || fail "clipboard backend: $(grep clipboard "$OUT/session.log")"
grep -q "input through $INPUT" "$OUT/session.log" || fail "input backend: $(grep input "$OUT/session.log")"
step "$COMPOSITOR: clipboard through $CLIPBOARD, input through $INPUT"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
"$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" hold < "$RUNTIME/phone.in" \
  > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
wait_for "the phone never connected" eval "devices | grep -q \"'$ID', 'E2E Phone', true\""

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
# KWin's EIS server has no ei_text, so libei types only what the keymap has.
if [[ $COMPOSITOR == kwin ]]; then TEXT="hello world"; else TEXT="héllo wörld"; fi
say "input {\"action\":\"text\",\"text\":\"$TEXT\"}"
wait_for "the text did not arrive: $(last text)" typed "$TEXT"
step "input: (+40, -25) moved the same, left click as button 1, \"$TEXT\" typed into the entry"
