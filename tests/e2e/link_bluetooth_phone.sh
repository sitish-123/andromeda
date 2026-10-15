#!/usr/bin/env bash
# Bluetooth and the hotspot handoff with real radios: the phone on RFCOMM to this laptop's BlueZ, and the laptop
# joining the phone's local-only hotspot through the real NetworkManager. The phone's Wi-Fi is off, so Bluetooth is
# its only path to the laptop until the hotspot.
#
# Proves:
# 1. With Wi-Fi off, the phone's session comes over Bluetooth (RFCOMM to Link's profile, TLS with the paired keys).
# 2. Over Bluetooth: a share reaches the phone and is acknowledged, a 512 KiB file arrives with a matching SHA-256,
#    and the phone's player artwork (512 px at the source) arrives cut to 128 px.
# 3. Mirroring asked over Bluetooth waits for the phone's hotspot: D-Bus signals Hotspot with the SSID before
#    NetworkManager takes the laptop off its Wi-Fi, the session moves to the hotspot, and frames decode.
# 4. With nothing moving, the phone ends the hotspot, the session drops back to Bluetooth, and the laptop is on its
#    own Wi-Fi again.
# 5. A 1 GiB file asks for the hotspot again and arrives over it with a matching SHA-256; join time and throughput
#    are recorded, and the laptop's Wi-Fi comes back after.
# Takes the laptop off its Wi-Fi for minutes. Stops the user's umbriel-linkd for the run (it holds Link's Bluetooth
# profile and port 4717) and starts it again after, re-enables the phone's Wi-Fi, and brings the laptop's Wi-Fi back
# even on failure. Writes results.jsonl, linkd.log, signals.txt, nm.txt, viewer.log, frames/ to $OUT
# (default ./artifacts/link-bluetooth-phone).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-bluetooth-phone}
source "$(dirname "$0")/link_phone_lib.sh"

[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /var/tmp/link-bluetooth-phone.XXXX)
HOME_WIFI=$(nmcli -t -f NAME,TYPE con show --active | grep ':802-11-wireless$' | head -1 | cut -d: -f1)
[[ -n $HOME_WIFI ]] || fail "the laptop is not on Wi-Fi"
USER_LINKD=$(systemctl --user is-active umbriel-linkd || true)
restore() {
  kill $(jobs -p) 2>/dev/null || true
  wait 2>/dev/null
  adb shell svc wifi enable || true
  nmcli -t -f NAME con show --active | grep -qxF "$HOME_WIFI" || nmcli con up id "$HOME_WIFI" > /dev/null || true
  [[ $USER_LINKD != active ]] || systemctl --user start umbriel-linkd
  rm -rf "$RUNTIME"
}
trap restore EXIT
[[ $(adb get-state 2>/dev/null) == device ]] || fail "no phone at $SERIAL"
[[ -s $APK && -s $FIXTURE ]] || fail "no APKs at $APK and $FIXTURE"
[[ $(adb get-devpath) == usb* ]] || fail "adb must run over USB: the phone's Wi-Fi goes off"

systemctl --user stop umbriel-linkd 2>/dev/null || true
# The session bus is the run's own; BlueZ and NetworkManager are the real ones on the system bus.
private_bus
start_linkd
[[ $PORT == 4717 ]] || fail "umbriel-linkd is not on Link's port 4717, which the firewall lets in: $PORT"
wait_for 10 "BlueZ did not take Link's profile: $(grep -i bluetooth "$OUT/linkd.log")" \
  grep -q 'Bluetooth: listening on' "$OUT/linkd.log"
( nmcli monitor | stamp > "$OUT/nm.txt" ) &

install_app
HOST=$(ip -4 route get 1.1.1.1 | grep -o 'src [0-9.]*' | cut -d' ' -f2)
pair_phone "$HOST:$PORT"
phone_switches screen
record "{\"step\":\"paired\",\"device\":\"$ID\",\"home_wifi\":\"$HOME_WIFI\"}"

on_bluetooth() { grep "connected over" "$OUT/linkd.log" | tail -1 | grep -q "over Bluetooth"; }
on_ip() { grep "connected over" "$OUT/linkd.log" | tail -1 | grep -q "over IP"; }
home_back() { nmcli -t -f NAME con show --active | grep -qxF "$HOME_WIFI"; }
line_ms() { grep "$1" "$OUT/linkd.log" | tail -1 | cut -d' ' -f1; }

T0=$(now_ms)
adb shell svc wifi disable
wait_for 120 "the phone did not come over Bluetooth" on_bluetooth
record "{\"step\":\"rfcomm-session\",\"after_wifi_off_ms\":$(( $(line_ms 'connected over Bluetooth') - T0 ))}"

T0=$(now_ms)
link Share "'$ID'" "'text'" "'over Bluetooth'" > /dev/null || fail "a share over Bluetooth"
SHARE_MS=$(( $(now_ms) - T0 ))

# accept_offer NAME: Accept on the phone's offer notification for NAME.
accept_offer() {
  wait_for 30 "no offer notification" eval 'adb shell dumpsys notification --noredact | grep -q "wants to send"'
  adb shell cmd statusbar expand-notifications
  tap_text "offer-$1" "*wants to send*"
  tap_text "accept-$1" Accept
  adb shell cmd statusbar collapse
}
# delivered NAME FILE: the phone's Downloads copy of NAME matches FILE.
delivered() {
  [[ $(adb shell sha256sum "/sdcard/Download/$1" 2>/dev/null | cut -d' ' -f1) == $(sha256sum < "$2" | cut -d' ' -f1) ]]
}
head -c 524288 /dev/urandom > "$RUNTIME/bt-small.bin"
T0=$(now_ms)
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/bt-small.bin" > /dev/null || fail "SendFiles 512 KiB"
accept_offer small
wait_for 120 "the 512 KiB file did not arrive intact" delivered bt-small.bin "$RUNTIME/bt-small.bin"
SMALL_MS=$(( $(now_ms) - T0 ))
on_bluetooth || fail "a 512 KiB send left Bluetooth"

adb shell am start -W -n org.umbriel.link.fixture/.PlayerActivity --ei art 512 > /dev/null
ART_DIR=$RUNTIME/desktop/umbriel-link/art
wait_for 30 "no artwork from the phone's player" eval 'ls "$ART_DIR"/* > /dev/null 2>&1'
ART=$(magick identify -format '%wx%h' "$(ls "$ART_DIR"/* | head -1)")
[[ $ART == 128x128 ]] || fail "artwork over Bluetooth is $ART"
adb shell am force-stop org.umbriel.link.fixture
record "{\"step\":\"over-bluetooth\",\"share_ms\":$SHARE_MS,\"file_512k_ms\":$SMALL_MS,\"artwork\":\"$ART\"}"
adb shell rm -f /sdcard/Download/bt-small.bin

# Mirroring on Bluetooth: the request waits for the hotspot.
T0=$(now_ms)
"$BIN/umbriel-link-mirror" "$ID" --frames "$OUT/frames" --count 120 > "$OUT/viewer.log" 2>&1 &
VIEWER=$!
wait_for 90 "the session did not move to the hotspot for mirroring" on_ip
JOINED=$(line_ms 'connected over IP')
allow_mirror hotspot
wait "$VIEWER" || fail "the viewer failed: $(tail -3 "$OUT/viewer.log")"
FRAMES=$(ls "$OUT/frames" | wc -l)
(( FRAMES >= 100 )) || fail "only $FRAMES frames over the hotspot"
NOTICE=$(grep -m1 'member=Hotspot' "$OUT/signals.txt" | grep -o 'time=[0-9]*\.[0-9]\{3\}' | cut -d= -f2 | tr -d .)
LEFT=$(grep -m1 -E "$HOME_WIFI.*(deactivat|disconnect)|wlan0: (deactivating|disconnected|disconnecting)" "$OUT/nm.txt" | cut -d' ' -f1)
[[ -n $NOTICE && -n $LEFT ]] || fail "no Hotspot signal ($NOTICE) or no Wi-Fi change ($LEFT)"
(( NOTICE <= LEFT )) || fail "the Hotspot notice came $(( NOTICE - LEFT )) ms after the laptop left its Wi-Fi"
record "{\"step\":\"mirror-moves-to-hotspot\",\"join_ms\":$(( JOINED - T0 )),\"notice_before_leaving_ms\":$(( LEFT - NOTICE )),\"frames\":$FRAMES}"

# Nothing moving: the hotspot idles out and everything returns.
T0=$(now_ms)
wait_for 180 "the session did not fall back to Bluetooth" on_bluetooth
wait_for 120 "the laptop did not return to $HOME_WIFI" home_back
record "{\"step\":\"hotspot-idles-out\",\"back_on_bluetooth_and_wifi_ms\":$(( $(now_ms) - T0 ))}"

head -c 1073741824 /dev/urandom > "$RUNTIME/hotspot-1g.bin"
T0=$(now_ms)
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/hotspot-1g.bin" > "$RUNTIME/transfer" \
  || fail "SendFiles 1 GiB"
wait_for 90 "the 1 GiB send did not move to the hotspot" on_ip
JOINED=$(line_ms 'connected over IP')
accept_offer big
TRANSFER=$(cat "$RUNTIME/transfer")
wait_for 600 "the 1 GiB transfer did not finish" eval 'grep -A2 "member=TransferFinished" "$OUT/signals.txt" | grep -q "\"$TRANSFER\""'
DONE=$(grep -A2 "member=TransferFinished" "$OUT/signals.txt" | grep -A1 "\"$TRANSFER\"" | grep -m1 -o '"done"' || true)
[[ -n $DONE ]] || fail "the 1 GiB transfer did not end done: $(grep -A3 "\"$TRANSFER\"" "$OUT/signals.txt" | tail -3)"
FINISHED=$(now_ms)
wait_for 120 "the 1 GiB file did not land intact" delivered hotspot-1g.bin "$RUNTIME/hotspot-1g.bin"
SECONDS_TAKEN=$(( (FINISHED - JOINED) / 1000 ))
record "{\"step\":\"1gib-over-hotspot\",\"join_ms\":$(( JOINED - T0 )),\"transfer_s\":$SECONDS_TAKEN,\"mbyte_per_s\":$(( 1024 / (SECONDS_TAKEN > 0 ? SECONDS_TAKEN : 1) ))}"
adb shell rm -f /sdcard/Download/hotspot-1g.bin
T0=$(now_ms)
wait_for 180 "the laptop did not return to $HOME_WIFI after the transfer" home_back
record "{\"step\":\"wifi-restored\",\"after_ms\":$(( $(now_ms) - T0 ))}"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
