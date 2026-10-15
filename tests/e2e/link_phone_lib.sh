# Shared by the real-phone E2E scripts (link_*_phone.sh): the phone lock, a private bus, a private umbriel-linkd, the
# debug app paired with it, and taps on Android's own UI through uiautomator. Source it after setting OUT.
# Needs ANDROID_SERIAL, the debug APKs, and the phone on USB debugging.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${LINK_BIN:-$ROOT/link/target/debug}
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
FIXTURE=${FIXTURE:-$ROOT/link/android/fixture/build/outputs/apk/debug/fixture-debug.apk}
PACKAGE=org.umbriel.link

fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}
adb() { "$ADB" -s "$SERIAL" "$@"; }
now_ms() { date +%s%3N; }
# stamp: each line of stdin with the Unix time in ms in front.
stamp() { while IFS= read -r line; do printf '%s %s\n' "$(date +%s%3N)" "$line"; done; }
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
prop() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 "$1"; }
devices() { prop Devices; }
connected() { devices | grep -q "', true)"; }

# private_bus: a session bus of the run's own; with "system", also its system bus.
private_bus() {
  cat > "$RUNTIME/bus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$RUNTIME/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
  dbus-daemon --config-file="$RUNTIME/bus.conf" --nofork 2> "$OUT/dbus.log" &
  for _ in $(seq 50); do [[ -S $RUNTIME/bus ]] && break; sleep 0.02; done
  export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus
  [[ ${1:-} == system ]] && export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/bus
  true
}

start_linkd() {
  mkdir -p "$RUNTIME/downloads"
  XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$RUNTIME/downloads RUST_LOG=info,zbus=warn,tracing=warn \
    "$BIN/umbriel-linkd" 2>&1 | stamp > "$OUT/linkd.log" &
  for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
  link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
  wait_for 5 "umbriel-linkd logged no port" grep -q 'listening on port' "$OUT/linkd.log"
  dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
  PORT=$(grep -o 'listening on port [0-9]*' "$OUT/linkd.log" | grep -o '[0-9]*$')
}

# install_app: Link replaced by the debug build (a release build's key differs, so it goes first), with its runtime
# permissions granted at install, since ColorOS refuses `pm grant`.
install_app() {
  adb shell input keyevent KEYCODE_WAKEUP
  adb shell cmd statusbar collapse
  # An update, not a fresh install: the phone keeps the pairings of other runs and of the user.
  adb install -r -g "$APK" > /dev/null || fail "installing $APK; a release build on the phone has another key"
  adb install -r "$FIXTURE" > /dev/null || fail "installing $FIXTURE"
  record '{"step":"installed"}'
}

ui() { adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1; adb shell cat /sdcard/ui.xml > "$OUT/ui-$1.xml"; }
# bounds NAME TEXT: the centre of the first node whose text or description is TEXT (any case), as "x y", from dump
# NAME; a prefix match with a trailing '*', a substring match with '*' at both ends.
bounds() {
  python3 - "$OUT/ui-$1.xml" "$2" <<'PY'
import re, sys
xml, want = open(sys.argv[1]).read(), sys.argv[2].lower()
for node in re.finditer(r'<node [^>]*>', xml):
    attrs = dict(re.findall(r'(\w[\w-]*)="([^"]*)"', node.group(0)))
    labels = [attrs.get("text", "").lower(), attrs.get("content-desc", "").lower()]
    def matches(label):
        if want.startswith("*") and want.endswith("*"):
            return want[1:-1] in label
        return label.startswith(want[:-1]) if want.endswith("*") else label == want
    if any(matches(label) for label in labels):
        l, t, r, b = map(int, re.findall(r"\d+", attrs["bounds"]))
        print((l + r) // 2, (t + b) // 2)
        break
PY
}
# tap_text NAME TEXT...: wait for any of TEXT on screen and tap it.
tap_text() {
  local name=$1; shift
  for _ in $(seq 40); do
    ui "$name"
    for text in "$@"; do
      local at
      at=$(bounds "$name" "$text")
      if [[ -n $at ]]; then adb shell input tap $at; return; fi
    done
    sleep 0.5
  done
  fail "none of $* appeared on screen"
}

# pair_phone ADDRESS: pairs the app with the private daemon at ADDRESS (host:port), plus its Bluetooth adapter when
# the daemon has one; sets ID.
pair_phone() {
  local uri
  uri=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/" | sed -E "s/&a=[^&]*//g")
  uri="$uri&a=$1"
  adb shell am start -W -a android.intent.action.VIEW -d "'$uri'" "$PACKAGE" > /dev/null
  # The clipboard watcher asks for log access when the app opens, which can cover the pairing prompt.
  for _ in 1 2; do
    tap_text pair "Allow one-time access" Pair
    [[ -n $(bounds pair "Allow one-time access") ]] || break
  done
  wait_for 30 "D-Bus does not show the phone connected: $(devices)" connected
  ID=$(devices | grep -o "'[0-9a-f]\{32\}'" | head -1 | tr -d "'")
  DESKTOP=$(phone_peers | tail -1)
}
# phone_peers: the ids of the desktops the phone is paired with, oldest first.
phone_peers() {
  adb shell run-as "$PACKAGE" cat files/devices.json \
    | python3 -c 'import json,sys; print(*(p["id"] for p in json.load(sys.stdin)["peers"]), sep="\n")'
}
# forget_phone: removes the run's desktop from the stopped app's store, and keeps every other pairing.
forget_phone() {
  [[ -n ${DESKTOP:-} ]] || return 0
  adb shell am force-stop "$PACKAGE"
  adb shell run-as "$PACKAGE" cat files/devices.json > "$RUNTIME/forget.json" || return 0
  python3 - "$RUNTIME/forget.json" "$DESKTOP" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"] = [peer for peer in store["peers"] if peer["id"] != sys.argv[2]]
json.dump(store, open(sys.argv[1], "w"))
PY
  adb shell run-as "$PACKAGE" sh -c "'cat > files/devices.json'" < "$RUNTIME/forget.json"
}

# phone_switches SWITCH...: turns those of the phone's switches for the desktop on, and Stay connected, with the app
# stopped, then starts it and waits for its new session.
phone_switches() {
  local sessions
  sessions=$(grep -c "connected over" "$OUT/linkd.log")
  adb shell am force-stop "$PACKAGE"
  adb shell run-as "$PACKAGE" cat files/devices.json > "$RUNTIME/phone.json"
  python3 - "$RUNTIME/phone.json" "$@" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
for switch in sys.argv[2:]:
    store["peers"][-1].setdefault("grants", {})[switch] = True
json.dump(store, open(sys.argv[1], "w"))
PY
  adb shell run-as "$PACKAGE" sh -c "'cat > files/devices.json'" < "$RUNTIME/phone.json"
  printf '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n<map><boolean name="stay_connected" value="true" /></map>\n' \
    | adb shell run-as "$PACKAGE" sh -c "'mkdir -p shared_prefs && cat > shared_prefs/presence.xml'"
  # A fresh task: the old one still holds the pairing link, which would open again.
  adb shell am start -W --activity-clear-task -n "$PACKAGE/.MainActivity" > /dev/null
  wait_for 30 "the app did not reconnect" eval '(( $(grep -c "connected over" "$OUT/linkd.log") > sessions ))'
}

# allow_mirror NAME: Allow on the request, as a heads-up or in the shade (ColorOS can keep a pinned heads-up out of
# the shade), then Start on Android's capture prompt. Goes home first: a fullscreen app can hide both.
allow_mirror() {
  wait_for 75 "no mirror request notification" eval 'adb shell dumpsys notification --noredact | grep -q "wants to show your screen"'
  adb shell input keyevent KEYCODE_HOME
  local at="" was="" try
  # The shade moves while it opens, so a position counts only once two dumps a second apart agree on it.
  for try in $(seq 16); do
    ui "notification-$1"
    at=$(bounds "notification-$1" Allow)
    [[ -n $at ]] || at=$(bounds "notification-$1" "*wants to show your screen*")
    [[ -z $at || $at != "$was" ]] || break
    was=$at
    if [[ -n $at ]]; then sleep 1; continue; fi
    if (( try % 2 )); then adb shell cmd statusbar expand-notifications; else adb shell cmd statusbar collapse; fi
    sleep 0.7
  done
  [[ -n $at && $at == "$was" ]] || fail "the mirror request is neither a heads-up nor in the shade"
  adb shell input tap $at
  tap_text "prompt-$1" "Start" "Start now" "Share screen"
  adb shell cmd statusbar collapse
}
