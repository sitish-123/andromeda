#!/usr/bin/env bash
# The phone as the desktop's remote, through the shell, against a real andromeda-linkd and a present headless phone, with
# a fullscreen GTK 4 window that logs what reaches it. Proves:
# 1. Remote input without the desktop's input grant is dropped; nothing reaches the window.
# 2. With the grant: moves arrive as the same relative motion, left and right clicks as buttons 1 and 3, a scroll as a
#    downward scroll, the presenter's keys (page-down, page-up, f5, escape) as those keysyms, and Unicode text as the
#    entry's text, through the shell's virtual pointer and keyboard.
# 3. Commands set over D-Bus reach the phone by name; running one by id runs its line here; an unknown id runs
#    nothing; with the commands grant off the phone gets an empty list and a run is dropped.
# 4. A phone's ping shows a desktop notification; the desktop's ping reaches the phone.
# 5. The Devices tab shows Ping and the new switches, and Settings, Phones, the Commands card.
# Every message is validated against link/api/messages.cddl. Writes steps.txt, window.jsonl, hold.jsonl,
# signals.txt, transcript.jsonl, linkd.log, noctalia.log, and *.png to $OUT (default ./artifacts/link-control).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-control}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
rm -rf "$OUT"
boot_headless 1

with_noctalia '
  BIN='"$LINK_BIN"'
  E2E='"$ROOT/tests/e2e"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" | tee -a "$OUT/steps.txt"; }
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
  phone() { "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  say() { printf "%s\n" "$*" >&3; }
  phone_saw() { [[ $(grep -c "$1" "$OUT/hold.jsonl") -ge ${2:-1} ]]; }
  logged() { grep -q "$1" "$OUT/linkd.log"; }
  seen() { cat "$OUT/window.jsonl" 2> /dev/null | grep -c "\"event\": \"$1\"" || true; }
  last() { grep "\"event\": \"$1\"" "$OUT/window.jsonl" | tail -1; }
  field() { python3 -c "import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])" "$1" "$2"; }
  input() { say "input $1"; }
  count() { (( $(seen "$1") >= $2 )); }
  at() { [[ $(field "$(last motion)" x) == "$1" && $(field "$(last motion)" y) == "$2" ]]; }
  released() { grep -q "\"release\", \"button\": $1" "$OUT/window.jsonl"; }
  keys() { grep "\"event\": \"key\"" "$OUT/window.jsonl" | tail -4 | sed -E "s/.*\"name\": \"([^\"]*)\".*/\1/" | tr "\n" " "; }
  pressed() { [[ $(keys) == "$1" ]]; }
  typed() { [[ "$(field "$(last text)" text)" == "$1" ]]; }
  listed() { grep "\"event\":\"commands\"" "$OUT/hold.jsonl" | tail -1 | grep -q "$1"; }
  pinged() { grep -A2 "member=Pinged" "$OUT/signals.txt" | grep -q "$1"; }

  XDG_STATE_HOME=$RUNTIME/linkd RUST_LOG=info,zbus=warn "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  dbus-monitor --session "type=signal,interface=org.andromeda.Link1" > "$OUT/signals.txt" 2>&1 &
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair"
  wait_for "no pairing window" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"E2E Phone\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" --transcript "$OUT/transcript.jsonl" \
    hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  wait_for "the phone got no command list on connect" phone_saw "\"event\":\"commands\""

  GDK_BACKEND=wayland python3 "$E2E/link_input_window.py" "$OUT/window.jsonl" 2> "$OUT/window.log" &
  probe=$!
  wait_for "the probe window did not start" count ready 1
  sleep 1 # real time: the window maps fullscreen and takes keyboard focus

  input "{\"action\":\"move\",\"dx\":30,\"dy\":30}"
  wait_for "input without the grant was not dropped" logged "dropped a remote-input its grant does not allow"
  sleep 0.5 # real time: a stray event would have arrived by now
  (( $(seen motion) == 0 )) || fail "input without the grant moved the pointer"
  step "remote input without the input grant: dropped, the window saw nothing"

  link SetGrant "$ID" input true > /dev/null
  input "{\"action\":\"move\",\"dx\":5,\"dy\":5}"
  wait_for "the first move did not reach the window" count motion 1
  sleep 0.3 # real time: the enter and first motion settle
  before=$(last motion)
  input "{\"action\":\"move\",\"dx\":40,\"dy\":-25}"
  x0=$(field "$before" x); y0=$(field "$before" y)
  wait_for "the move did not arrive as (+40, -25) from ($x0, $y0): $(last motion)" at $(( x0 + 40 )) $(( y0 - 25 ))
  step "moves: (+40, -25) arrived as the same relative motion, ($x0, $y0) to ($(( x0 + 40 )), $(( y0 - 25 )))"

  input "{\"action\":\"move\",\"dx\":0,\"dy\":300}"
  input "{\"action\":\"click\",\"button\":\"left\"}"
  wait_for "no left click" released 1
  input "{\"action\":\"click\",\"button\":\"right\"}"
  wait_for "no right click" released 3
  input "{\"action\":\"scroll\",\"dx\":0,\"dy\":60}"
  wait_for "no scroll" count scroll 1
  dy=$(field "$(last scroll)" dy)
  python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0 else 1)" "$dy" || fail "the scroll went up: $dy"
  step "buttons: left click as button 1, right click as button 3; scroll dy 60 arrived as dy $dy"

  # A headless session has no keyboard until the shell makes its virtual one, and the window binds it only then, so a
  # first key may arrive before the window listens.
  input "{\"action\":\"key\",\"key\":\"escape\"}"
  sleep 1 # real time: the window binds the new keyboard
  for key in page-down page-up f5 escape; do
    input "{\"action\":\"key\",\"key\":\"$key\"}"
  done
  wait_for "the presenter keys did not all arrive: $(keys)" pressed "Page_Down Page_Up F5 Escape "
  step "presenter keys: page-down, page-up, f5, escape arrived as Page_Down, Page_Up, F5, Escape"

  TEXT="héllo wörld ✓"
  input "{\"action\":\"text\",\"text\":\"$TEXT\"}"
  wait_for "the text did not arrive: $(last text)" typed "$TEXT"
  step "text: \"$TEXT\" typed into the entry"

  link SetCommands "[(\"Touch the file\", \"touch $RUNTIME/ran\"), (\"Say hi\", \"true\")]" > /dev/null
  wait_for "the phone did not get the two commands" listed "Say hi"
  COMMAND=$(grep "\"event\":\"commands\"" "$OUT/hold.jsonl" | tail -1 | python3 -c "import json,sys; print(json.load(sys.stdin)[\"commands\"][0][\"id\"])")
  say "run $COMMAND"
  wait_for "the command did not run" test -e "$RUNTIME/ran"
  say "run 0000000000000000"
  wait_for "an unknown command was not refused" logged "no command 0000000000000000"
  step "commands: the phone got both names; running $COMMAND ran its line; an unknown id ran nothing"

  rm "$RUNTIME/ran"
  link SetGrant "$ID" commands false > /dev/null
  wait_for "the phone did not get an empty list" listed "\"commands\":\[\]"
  say "run $COMMAND"
  wait_for "a run without the grant was not dropped" logged "dropped a run-command its grant does not allow"
  [[ ! -e $RUNTIME/ran ]] || fail "a command ran without the grant"
  link SetGrant "$ID" commands true > /dev/null
  step "commands grant off: the phone got an empty list, and a run was dropped"

  msg panel-close > /dev/null
  say "ping Lunch is ready"
  wait_for "no Pinged signal" pinged "Lunch is ready"
  sleep 1.5 # real time: the toast maps and paints
  grim "$OUT/ping-toast.png"
  [[ $(msg notification-dismiss-latest) == ok ]] || fail "no ping notification to dismiss"
  link Ping "$ID" "Come back" > /dev/null
  wait_for "the phone did not get the ping" phone_saw "\"event\":\"ping\".*Come back"
  step "pings: the phone pinged the desktop, which showed a notification; the desktop pinged the phone"

  kill "$probe" # the fullscreen window would cover the panel and the settings window
  msg panel-open control-center devices > /dev/null
  sleep 1 # real time: the tab lays out
  grim "$OUT/devices-tab.png"
  msg panel-close > /dev/null
  msg settings-open devices > /dev/null
  sleep 1.5 # real time: the window maps and lays out
  grim "$OUT/settings-commands.png"
  msg settings-close > /dev/null
  step "screens: devices-tab.png (Ping, input and commands switches), settings-commands.png (Commands card)"
  kill "$hold"
  wait "$hold" || true
'
"$LINK_BIN/andromeda-link-phone" check-transcript "$ROOT/link/api/messages.cddl" "$OUT/transcript.jsonl" \
  | tee "$OUT/transcript-check.json" || { echo "FAIL: the transcript violates the schema" >&2; exit 1; }
for shot in ping-toast devices-tab settings-commands; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
