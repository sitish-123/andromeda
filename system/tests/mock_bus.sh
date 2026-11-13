#!/usr/bin/env bash
# andromeda-system against tests/e2e/mock_power.py on a private bus: state follows the mock within 1 s, the user's
# choices reach the backends, and a stopped backend shows as unavailable and comes back after a restart.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${ANDROMEDA_SYSTEM:-$ROOT/system/target/debug/andromeda-system}
OUT=$(mktemp -d /tmp/system-mock.XXXXXX)
PIDS=()
cleanup() { for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "logs: $OUT" >&2; exit 1; }

# Polls a command until its output contains the expected text; deadline in tenths of a second.
until_has() {
  local deadline=$1 expected=$2 out=""; shift 2
  for _ in $(seq "$deadline"); do
    out=$("$@" 2>&1 || true)
    [[ $out == *"$expected"* ]] && return 0
    read -rt 0.1 <> <(:) || true
  done
  fail "$* gave '$out', wanted '$expected'"
}

dbus-daemon --session --nofork --print-address=3 3>"$OUT/addr" & PIDS+=($!)
until_has 50 unix: cat "$OUT/addr"
export DBUS_SYSTEM_BUS_ADDRESS
DBUS_SYSTEM_BUS_ADDRESS=$(head -n1 "$OUT/addr")
B=(busctl --address="$DBUS_SYSTEM_BUS_ADDRESS")
get() { "${B[@]}" get-property org.andromeda.System1 "$1" "org.andromeda.System1.$2" "$3"; }
BAT=/org/andromeda/System1/Battery/battery_BAT0

start_mock() {
  MOCK_BATTERY=1 python3 "$ROOT/tests/e2e/mock_power.py" "$OUT/mock.log" >"$OUT/mock.out" 2>&1 & MOCK=$!
  PIDS+=("$MOCK")
  until_has 50 ready cat "$OUT/mock.out"
}
start_mock
# The fake nl80211 file (wifi.rs Fake): client wlan0, ifindex 3, card 0, station (2), up, on 2412 MHz.
export ANDROMEDA_SYSTEM_FAKE_WIFI=$OUT/wifi
echo 'wlan0 3 0 2 1 2412' >"$ANDROMEDA_SYSTEM_FAKE_WIFI"
STATE_DIRECTORY=$OUT ANDROMEDA_SYSTEM_TEST=1 "$BIN" >"$OUT/daemon.log" 2>&1 & PIDS+=($!)

until_has 30 '"battery" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"power" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"memory" true ""' get /org/andromeda/System1 Manager Areas
# GLib apps and the portal hear memory pressure through low-memory-monitor's name (not installed here).
if [ ! -e /usr/share/dbus-1/system-services/org.freedesktop.LowMemoryMonitor.service ]; then
  until_has 10 LowMemoryWarning "${B[@]}" introspect org.freedesktop.LowMemoryMonitor /org/freedesktop/LowMemoryMonitor
fi
until_has 10 '"storage" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"thermal" true' get /org/andromeda/System1 Manager Areas
until_has 10 '"network" false "org.freedesktop.NetworkManager' get /org/andromeda/System1 Manager Areas
until_has 10 'd 64' get "$BAT" Battery Percent
until_has 10 's "battery"' get "$BAT" Battery Kind
until_has 10 '(bbuuu) true false 75 80 3' get "$BAT" Battery ChargeLimit
until_has 10 's "balanced"' get /org/andromeda/System1/Power Power Profile
"${B[@]}" call org.andromeda.System1 /org/andromeda/System1 org.freedesktop.DBus.ObjectManager GetManagedObjects \
  | grep -q battery_BAT0 || fail "GetManagedObjects lacks the battery"

"${B[@]}" call org.freedesktop.UPower /org/freedesktop/UPower dsk.test.Power SetBattery du 15 2
until_has 10 'd 15' get "$BAT" Battery Percent
"${B[@]}" call org.freedesktop.UPower /org/freedesktop/UPower dsk.test.Power SetOnBattery b true
until_has 10 'b true' get /org/andromeda/System1/Power Power OnBattery

"${B[@]}" call org.andromeda.System1 /org/andromeda/System1/Knobs org.andromeda.System1.Knobs Set sv \
  power.profile s performance
until_has 10 's "performance"' get /org/andromeda/System1/Power Power Profile
grep -qx 'ActiveProfile=performance' "$OUT/mock.log" || fail "profile did not reach the backend"
# A client hold wins while its owner stays on the bus; the owner's exit or a user change ends it.
hold() {
  python3 -c 'import sys, time, dbus
knobs = dbus.bus.BusConnection(sys.argv[1]).get_object("org.andromeda.System1", "/org/andromeda/System1/Knobs")
print(knobs.Hold("power.profile", dbus.String(sys.argv[2], variant_level=1), "power.low_power_plan",
                 dbus_interface="org.andromeda.System1.Knobs"), flush=True)
time.sleep(60)' "$DBUS_SYSTEM_BUS_ADDRESS" "$1" >"$OUT/hold.out" 2>&1 & HOLDER=$!
  PIDS+=("$HOLDER")
}
LIST=(call org.andromeda.System1 /org/andromeda/System1/Knobs org.andromeda.System1.Knobs List)
hold power-saver
until_has 10 's "power-saver"' get /org/andromeda/System1/Power Power Profile
until_has 10 '"power-saver" "automatic"' "${B[@]}" "${LIST[@]}"
kill "$HOLDER"
until_has 10 's "performance"' get /org/andromeda/System1/Power Power Profile
grep -qx 'Release=power-saver' "$OUT/mock.log" || fail "the hold outlived its owner"
hold power-saver
until_has 10 's "power-saver"' get /org/andromeda/System1/Power Power Profile
"${B[@]}" call org.andromeda.System1 /org/andromeda/System1/Knobs org.andromeda.System1.Knobs Set sv \
  power.profile s balanced
until_has 10 's "balanced"' get /org/andromeda/System1/Power Power Profile
until_has 10 's "balanced" 0' "${B[@]}" "${LIST[@]}"
kill "$HOLDER"
until_has 10 'aa{sv} 0' "${B[@]}" call org.andromeda.System1 /org/andromeda/System1/Explain \
  org.andromeda.System1.Explain List t 0
"${B[@]}" call org.andromeda.System1 "$BAT" org.andromeda.System1.Battery SetChargeLimit buu true 4294967295 4294967295
until_has 10 '(bbuuu) true true 75 80 3' get "$BAT" Battery ChargeLimit
grep -qx 'EnableChargeThreshold=True' "$OUT/mock.log" || fail "charge limit did not reach the backend"
# battery.optimized_charging's hold: the limit goes on while the holder stays, then back to the user's choice.
until_has 10 'b true' get /org/andromeda/System1/Power Power ChargeLimitSupported
"${B[@]}" call org.andromeda.System1 "$BAT" org.andromeda.System1.Battery SetChargeLimit buu false 4294967295 4294967295
until_has 10 '(bbuuu) true false 75 80 3' get "$BAT" Battery ChargeLimit
python3 -c 'import sys, time, dbus
knobs = dbus.bus.BusConnection(sys.argv[1]).get_object("org.andromeda.System1", "/org/andromeda/System1/Knobs")
print(knobs.Hold("battery.charge_limit", dbus.Boolean(True, variant_level=1), "battery.optimized_charging",
                 dbus_interface="org.andromeda.System1.Knobs"), flush=True)
time.sleep(60)' "$DBUS_SYSTEM_BUS_ADDRESS" >"$OUT/hold-limit.out" 2>&1 & HOLDER=$!
PIDS+=("$HOLDER")
until_has 10 '(bbuuu) true true 75 80 3' get "$BAT" Battery ChargeLimit
kill "$HOLDER"
until_has 10 '(bbuuu) true false 75 80 3' get "$BAT" Battery ChargeLimit

kill "$MOCK"
until_has 10 '"battery" false "org.freedesktop.UPower stopped"' get /org/andromeda/System1 Manager Areas
until_has 10 '"power" false "org.freedesktop.UPower.PowerProfiles stopped"' get /org/andromeda/System1 Manager Areas
"${B[@]}" introspect org.andromeda.System1 "$BAT" >/dev/null 2>&1 && fail "battery object stayed after UPower stopped"

start_mock
until_has 30 '"battery" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"power" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 'd 64' get "$BAT" Battery Percent
KBD=/org/andromeda/System1/KbdBacklight/kbd_backlight
until_has 10 'i 1' get "$KBD" KbdBacklight Brightness
"${B[@]}" call org.andromeda.System1 "$KBD" org.andromeda.System1.KbdBacklight SetBrightness i 9
until_has 10 'i 3' get "$KBD" KbdBacklight Brightness

# RAPL and memory samples run only while watched; the watch ends with its caller.
"${B[@]}" call org.andromeda.System1 /org/andromeda/System1 org.andromeda.System1.Manager Watch as 2 power memory
"${B[@]}" call org.andromeda.System1 /org/andromeda/System1/Storage org.freedesktop.DBus.Properties Get ss \
  org.andromeda.System1.Storage Mounts | grep -q '"/"' || fail "Storage lacks /"
# The API matches the committed contract; UPDATE_CONTRACT=1 rewrites it after a deliberate change.
for path in / /Power /Memory /Network /Storage /Thermal /Display /Diagnostics /Knobs /Explain /Login /Battery/battery_BAT0 /KbdBacklight/kbd_backlight; do
  "${B[@]}" --xml-interface introspect org.andromeda.System1 "/org/andromeda/System1${path%/}" >"$OUT/introspect${path//\//_}.xml"
done
python3 "$ROOT/system/tests/contract.py" "$OUT"/introspect*.xml >"$OUT/contract.xml"
CONTRACT=$ROOT/system/data/org.andromeda.System1.xml
if [[ ${UPDATE_CONTRACT:-0} == 1 ]]; then cp "$OUT/contract.xml" "$CONTRACT"; fi
diff -u "$CONTRACT" "$OUT/contract.xml" || fail "API differs from $CONTRACT"
# Login forwards to logind (tests/mock_login1.py; the test bus has no polkit) by logind's rules: a block inhibitor,
# the caller's own too, refuses a power action unless the caller skips inhibitors; another user's block-weak one
# needs no skip; logind gets the skip flag only for a skip the caller asked for, else checks new inhibitors itself.
# Brightness goes only to the caller's session.
INH=$OUT/inhibitors
echo "shutdown block 0" >"$INH"
python3 "$ROOT/system/tests/mock_login1.py" "$OUT/login1.log" "$INH" >"$OUT/login1.out" 2>&1 & PIDS+=($!)
until_has 50 ready cat "$OUT/login1.out"
L=(call org.andromeda.System1 /org/andromeda/System1/Login org.andromeda.System1.Login)
until_has 10 's "yes"' "${B[@]}" "${L[@]}" CanPower s suspend
until_has 10 's "na"' "${B[@]}" "${L[@]}" CanPower s reboot-to-firmware-setup
"${B[@]}" "${L[@]}" Power sbb reboot true false 2>/dev/null && fail "reboot ran past a block inhibitor"
"${B[@]}" "${L[@]}" Power sbb reboot false true
echo "sleep block $(id -u)" >"$INH"
"${B[@]}" "${L[@]}" Power sbb suspend true false 2>/dev/null && fail "suspend ran past the caller's own block inhibitor"
echo "sleep block-weak $(id -u)" >"$INH"
"${B[@]}" "${L[@]}" Power sbb suspend true false
echo "sleep block-weak 4242" >"$INH"
"${B[@]}" "${L[@]}" Power sbb suspend true false
: >"$INH"
"${B[@]}" "${L[@]}" Power sbb suspend true true
"${B[@]}" "${L[@]}" Inhibit ssss handle-power-key Andromeda test block >/dev/null 2>&1 \
  && fail "System1 took a handle-power-key inhibitor, which logind would apply in every session"
"${B[@]}" "${L[@]}" Inhibit ssss sleep Andromeda test delay | grep -q '^h ' || fail "Inhibit returned no fd"
"${B[@]}" "${L[@]}" ListSessions | grep -q '^a(susso) 2 ' || fail "ListSessions"
"${B[@]}" "${L[@]}" SetBrightness sssu "" backlight intel 5
"${B[@]}" "${L[@]}" SetBrightness sssu c2 backlight intel 6 2>/dev/null && fail "SetBrightness reached another user's session"
# System1 relays PrepareForSleep only from logind, not from another peer, and the names of changed mirrored properties.
dbus-monitor --address "$DBUS_SYSTEM_BUS_ADDRESS" "type='signal',path='/org/andromeda/System1/Login'" \
  "type='signal',member='Marker'" >"$OUT/login-signals" 2>&1 & PIDS+=($!)
until_has 50 Marker sh -c "busctl --address=$DBUS_SYSTEM_BUS_ADDRESS emit /test test.T Marker; cat $OUT/login-signals"
"${B[@]}" emit /org/freedesktop/login1 org.freedesktop.login1.Manager PrepareForSleep b true
"${B[@]}" "${L[@]}" Power sbb suspend true false
until_has 30 'member=PrepareForSleep' cat "$OUT/login-signals"
[[ $(grep -c 'member=PrepareForSleep' "$OUT/login-signals") == 1 ]] || fail "System1 relayed a PrepareForSleep not from logind"
"${B[@]}" "${L[@]}" ScheduleShutdown stb halt 1 false 2>/dev/null && fail "ScheduleShutdown took an action other than poweroff or reboot"
"${B[@]}" "${L[@]}" ScheduleShutdown stb poweroff 123 false
until_has 30 '"ScheduledShutdown"' cat "$OUT/login-signals"
until_has 10 '"poweroff" 123' "${B[@]}" "${L[@]}" ManagerProperties
"${B[@]}" "${L[@]}" ManagerProperties | grep -q NAutoVTs && fail "ManagerProperties returned an unmirrored property"
"${B[@]}" "${L[@]}" CancelScheduledShutdown b false | grep -q '^b true' || fail "CancelScheduledShutdown"
"${B[@]}" "${L[@]}" CancelScheduledShutdown b false | grep -q '^b false' || fail "nothing scheduled must answer false"
diff -u - "$OUT/login1.log" <<'EOF' || fail "logind calls differ"
RebootWithFlags 16
SuspendWithFlags 1
SuspendWithFlags 0
SuspendWithFlags 1
Inhibit sleep delay
SetBrightness /session/c1 backlight intel 5
SuspendWithFlags 1
ScheduleShutdown poweroff 123
CancelScheduledShutdown
EOF
# The session part: the low battery rule holds power-saver through the system part, alerts once, and the alert's
# Undo releases the hold.
dbus-daemon --session --nofork --print-address=3 3>"$OUT/session-addr" & PIDS+=($!)
until_has 50 unix: cat "$OUT/session-addr"
SESSION_BUS=$(head -n1 "$OUT/session-addr")
python3 "$ROOT/system/tests/mock_notifications.py" "$OUT/notes.log" "$SESSION_BUS" >"$OUT/notes.out" 2>&1 & PIDS+=($!)
until_has 50 ready cat "$OUT/notes.out"
# xdg-open of the captive portal check: recorded, no browser.
mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho "$@" >> "%s/opened.log"\n' "$OUT" > "$OUT/bin/xdg-open"
chmod +x "$OUT/bin/xdg-open"
PATH=$OUT/bin:$PATH DBUS_SESSION_BUS_ADDRESS=$SESSION_BUS XDG_STATE_HOME=$OUT XDG_CONFIG_HOME=$OUT \
  ANDROMEDA_RULES_DIR=$ROOT/system/data/rules "$BIN" --session >"$OUT/session.log" 2>&1 & PIDS+=($!)
S=(busctl --address="$SESSION_BUS")
until_has 30 'aa{sv}' "${S[@]}" call org.andromeda.System1 /org/andromeda/System1/Explain \
  org.andromeda.System1.Explain List t 0
"${B[@]}" call org.freedesktop.UPower /org/freedesktop/UPower dsk.test.Power SetBattery du 15 2
"${B[@]}" call org.freedesktop.UPower /org/freedesktop/UPower dsk.test.Power SetOnBattery b true
until_has 20 's "power-saver"' get /org/andromeda/System1/Power Power Profile
until_has 10 'notify 1 Low Power Mode is on undo,Undo power.low_power_plan' cat "$OUT/notes.log"
until_has 10 '"started"' "${S[@]}" call org.andromeda.System1 /org/andromeda/System1/Explain \
  org.andromeda.System1.Explain List t 0
"${S[@]}" call org.freedesktop.Notifications /org/freedesktop/Notifications dsk.test.Notifications Invoke us 1 undo
until_has 10 's "balanced"' get /org/andromeda/System1/Power Power Profile
until_has 10 '"undone"' "${S[@]}" call org.andromeda.System1 /org/andromeda/System1/Explain \
  org.andromeda.System1.Explain List t 0
grep -c '^notify' "$OUT/notes.log" | grep -qx 1 || fail "the rule alerted more than once in one episode"
# Settings rows: the rules list, and a threshold change stored in the user's settings file.
RULES=(call org.andromeda.System1 /org/andromeda/System1/Rules org.andromeda.System1.Rules)
until_has 10 '"power.low_power_plan" true ""' "${S[@]}" "${RULES[@]}" List
# Every shipped rule loads: an empty third field is no load error.
for rule in apps.drainer memory.relief thermal.relief thermal.fps_cap thermal.hot storage.low battery.optimized_charging \
  storage.drive_failing storage.fs_errors network.captive_portal power.wakeups memory.leak apps.crashed \
  services.failed startup.slow_boot startup.slow_login battery.worn; do
  until_has 10 "\"$rule\" true \"\"" "${S[@]}" "${RULES[@]}" List
done
"${S[@]}" "${RULES[@]}" Set sba{sv} power.low_power_plan true 1 low_pct d 10
grep -q 'low_pct = 10' "$OUT/andromeda/system-rules.toml" || fail "the threshold was not stored"
"${S[@]}" "${RULES[@]}" Set sba{sv} power.low_power_plan false 0
until_has 10 '"power.low_power_plan" false ""' "${S[@]}" "${RULES[@]}" List
# network.captive_portal (§12.1): a portal opens NetworkManager's check URL once and alerts with Sign In.
python3 "$ROOT/system/tests/mock_nm.py" "$OUT/nm.log" >"$OUT/nm.out" 2>&1 & PIDS+=($!)
until_has 50 ready cat "$OUT/nm.out"
until_has 30 '"network" true ""' get /org/andromeda/System1 Manager Areas
"${B[@]}" call org.freedesktop.NetworkManager /org/freedesktop/NetworkManager dsk.test.NM SetConnectivity u 2
until_has 80 'Sign in to the network default,Sign In network.captive_portal' cat "$OUT/notes.log"
until_has 20 'http://nmcheck.test/check' cat "$OUT/opened.log"

# Network.CreateApInterface: wpa_supplicant (tests/mock_wpa_supplicant.py) makes wlan0ap; NetworkManager lists it
# (mock_nm.py). A second call after NetworkManager reset it to station and down makes it an access point and up
# again without a new interface; removal goes through wpa_supplicant. One caller holds the interface throughout.
python3 "$ROOT/system/tests/mock_wpa_supplicant.py" "$ANDROMEDA_SYSTEM_FAKE_WIFI" "$OUT/wpa.log" >"$OUT/wpa.out" 2>&1 &
PIDS+=($!)
until_has 50 ready cat "$OUT/wpa.out"
python3 -c 'import re, sys, dbus
wifi = sys.argv[2]
net = dbus.Interface(dbus.bus.BusConnection(sys.argv[1]).get_object("org.andromeda.System1",
                     "/org/andromeda/System1/Network"), "org.andromeda.System1.Network")
def row():
    return next((line.split() for line in open(wifi) if line.startswith("wlan0ap ")), None)
def check(what):
    r = row()
    if r is None or r[2:5] != ["0", "3", "1"]:
        sys.exit(f"{what}: wlan0ap is no access point that is up on card 0: {open(wifi).read()!r}")
    return r[1]
assert net.CreateApInterface("wlan0", timeout=20) == "wlan0ap"
first = check("create")
text = open(wifi).read()
open(wifi, "w").write(re.sub(r"^(wlan0ap \d+ 0) 3 1 ", r"\1 2 0 ", text, flags=re.M))
assert net.CreateApInterface("wlan0", timeout=20) == "wlan0ap"
if check("re-arm") != first:
    sys.exit("re-arm made a new interface")
net.RemoveApInterface("wlan0ap", timeout=20)
if row() is not None:
    sys.exit("wlan0ap stayed after RemoveApInterface")
' "$DBUS_SYSTEM_BUS_ADDRESS" "$ANDROMEDA_SYSTEM_FAKE_WIFI" || fail "access-point interface"
diff -u - "$OUT/wpa.log" <<'EOF' || fail "wpa_supplicant calls differ"
CreateInterface wlan0ap ap True
GetInterface wlan0ap
RemoveInterface /fi/w1/wpa_supplicant1/Interfaces/4
EOF
# Network NetworkManager writes check the caller first (tests/mock_polkit.py): a bad path before polkit, a connection
# for another user before polkit, then NetworkManager's polkit action; only then does NetworkManager see the call,
# and its errors come back unchanged.
DENY=$OUT/polkit-deny
echo org.freedesktop.NetworkManager.network-control >"$DENY"
HOLD=$OUT/polkit-hold
python3 "$ROOT/system/tests/mock_polkit.py" "$OUT/polkit.log" "$DENY" "$HOLD" >"$OUT/polkit.out" 2>&1 & PIDS+=($!)
until_has 50 ready cat "$OUT/polkit.out"
N=(call org.andromeda.System1 /org/andromeda/System1/Network org.andromeda.System1.Network)
NMO=/org/freedesktop/NetworkManager
"${B[@]}" "${N[@]}" DeactivateConnection o $NMO/ActiveConnection/1 2>/dev/null \
  && fail "DeactivateConnection ran without network-control"
: >"$DENY"
"${B[@]}" "${N[@]}" DeactivateConnection o $NMO/Settings/1 2>/dev/null \
  && fail "DeactivateConnection took a connection path as active connection"
"${B[@]}" "${N[@]}" DeactivateConnection o $NMO/ActiveConnection/2 2>/dev/null \
  && fail "DeactivateConnection stopped another user's connection"
"${B[@]}" "${N[@]}" DeactivateConnection o $NMO/ActiveConnection/1 || fail "DeactivateConnection"
dbus-send --bus="$DBUS_SYSTEM_BUS_ADDRESS" --print-reply --dest=org.andromeda.System1 /org/andromeda/System1/Network \
  org.andromeda.System1.Network.DeactivateConnection objpath:$NMO/ActiveConnection/3 \
  >"$OUT/nm-error" 2>&1 && fail "DeactivateConnection of an inactive connection"
grep -qx 'Error org.freedesktop.NetworkManager.ConnectionNotActive: not active' "$OUT/nm-error" \
  || fail "NetworkManager's error did not reach the caller: $(cat "$OUT/nm-error")"
diff -u - "$OUT/polkit.log" <<'EOF' || fail "polkit checks differ"
org.freedesktop.NetworkManager.network-control
org.freedesktop.NetworkManager.network-control
org.freedesktop.NetworkManager.network-control
EOF
diff -u - "$OUT/nm.log" <<EOF || fail "NetworkManager calls differ"
DeactivateConnection $NMO/ActiveConnection/1
DeactivateConnection $NMO/ActiveConnection/3
EOF
# A pending polkit dialog blocks only its own call: NetworkManager's connectivity still reaches Network, and another
# client reads it at once.
echo org.freedesktop.NetworkManager.network-control >"$HOLD"
"${B[@]}" --timeout=30 "${N[@]}" DeactivateConnection o $NMO/ActiveConnection/1 >"$OUT/held.out" 2>&1 &
HELD=$!
PIDS+=("$HELD")
until_has 50 4 grep -c '' "$OUT/polkit.log"
"${B[@]}" call org.freedesktop.NetworkManager /org/freedesktop/NetworkManager dsk.test.NM SetConnectivity u 4
until_has 30 's "full"' timeout 1 "${B[@]}" get-property org.andromeda.System1 /org/andromeda/System1/Network \
  org.andromeda.System1.Network Connectivity
: >"$HOLD"
wait "$HELD" || fail "the held DeactivateConnection: $(cat "$OUT/held.out")"
# Drives.PowerOff (tests/mock_udisks.py) checks the caller first: a bad path or option before polkit, then udisks2's
# own power-off-drive action by seat and system hint. Only then does udisks2 see the call.
python3 "$ROOT/system/tests/mock_udisks.py" "$OUT/udisks.log" >"$OUT/udisks.out" 2>&1 & PIDS+=($!)
until_has 50 ready cat "$OUT/udisks.out"
: >"$OUT/polkit.log"
D=(call org.andromeda.System1 /org/andromeda/System1/Storage org.andromeda.System1.Drives)
UD=/org/freedesktop/UDisks2
echo org.freedesktop.udisks2.power-off-drive >"$DENY"
"${B[@]}" "${D[@]}" PowerOff oa{sv} $UD/block_devices/sdb 0 2>/dev/null && fail "PowerOff took a block path"
"${B[@]}" "${D[@]}" PowerOff oa{sv} $UD/drives/Stick 1 force b true 2>/dev/null && fail "PowerOff took an unknown option"
"${B[@]}" "${D[@]}" PowerOff oa{sv} $UD/drives/Stick 0 2>/dev/null && fail "PowerOff ran without power-off-drive"
: >"$DENY"
for drive in Stick Internal Far; do
  "${B[@]}" "${D[@]}" PowerOff oa{sv} $UD/drives/$drive 0 || fail "PowerOff $drive"
done
diff -u - "$OUT/polkit.log" <<'EOF' || fail "udisks2 polkit checks differ"
org.freedesktop.udisks2.power-off-drive
org.freedesktop.udisks2.power-off-drive
org.freedesktop.udisks2.power-off-drive-system
org.freedesktop.udisks2.power-off-drive-other-seat
EOF
diff -u - "$OUT/udisks.log" <<EOF || fail "udisks2 calls differ"
PowerOff $UD/drives/Stick
PowerOff $UD/drives/Internal
PowerOff $UD/drives/Far
EOF
echo PASS
