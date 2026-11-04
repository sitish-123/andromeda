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
STATE_DIRECTORY=$OUT ANDROMEDA_SYSTEM_TEST=1 "$BIN" >"$OUT/daemon.log" 2>&1 & PIDS+=($!)

until_has 30 '"battery" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"power" true ""' get /org/andromeda/System1 Manager Areas
until_has 10 '"memory" true ""' get /org/andromeda/System1 Manager Areas
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
for path in / /Power /Memory /Network /Storage /Thermal /Knobs /Explain /Battery/battery_BAT0 /KbdBacklight/kbd_backlight; do
  "${B[@]}" --xml-interface introspect org.andromeda.System1 "/org/andromeda/System1${path%/}" >"$OUT/introspect${path//\//_}.xml"
done
python3 "$ROOT/system/tests/contract.py" "$OUT"/introspect*.xml >"$OUT/contract.xml"
CONTRACT=$ROOT/system/data/org.andromeda.System1.xml
if [[ ${UPDATE_CONTRACT:-0} == 1 ]]; then cp "$OUT/contract.xml" "$CONTRACT"; fi
diff -u "$CONTRACT" "$OUT/contract.xml" || fail "API differs from $CONTRACT"
echo PASS
