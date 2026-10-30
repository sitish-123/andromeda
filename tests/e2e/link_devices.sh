#!/usr/bin/env bash
# The shell's phone surfaces against a real andromeda-linkd started after the shell, andromeda-link-session, and a
# headless stand-in phone: link-devices refuses while the daemon is absent and answers once it owns its name; the
# phone pairs by code; the bar item and its popover show the paired phone's last-seen time from DeviceInfo; with the
# phone present (42%, charging, Wi-Fi, ringer vibrate, a playing player, one notification) the bar item, its popover,
# the quick settings phone tile, and the lock screen row show it; the row also counts a notification held while
# locked; link-unpair removes the phone and the bar item; stopping the daemon makes the shell report it gone. Writes
# bar-last-seen.png, popover-last-seen.png, bar.png, popover.png, quick-settings.png, lock-screen.png,
# lock-screen-held.png, steps.txt, hold.jsonl, linkd.log, session.log, and phone.log to $OUT (default
# ./artifacts/link-devices).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-devices}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
boot_headless 1
# The dashboard shows at most 6 tiles and the default set fills them, so the phone tile is added here.
printf '\n[[control_center.shortcuts]]\ntype = "phone"\n[[control_center.shortcuts]]\ntype = "wifi"\n' >> "$RUNTIME/home/.config/noctalia/config.toml"
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl" "$OUT/notify.txt"

with_noctalia '
  BIN='"$LINK_BIN"'
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  # wait_for DESCRIPTION CMD...: poll a shell IPC answer, which follows D-Bus signals asynchronously.
  wait_for() {
    local what=$1; shift
    for _ in $(seq 150); do "$@" && return; sleep 0.1; done
    fail "$what"
  }
  phone() { "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" "$@" 2>> "$OUT/phone.log"; }
  say() { printf "%s\n" "$*" >&3; }
  prop() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 "$1"; }
  shot() { sleep 1.5; grim "$OUT/$1.png"; } # real time: the surface maps and paints

  [[ $(msg link-devices) == *"not running"* ]] || fail "link-devices answered without a daemon: $(msg link-devices)"
  step "no daemon: $(msg link-devices)"

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  linkd=$!
  wait_for "shell never saw andromeda-linkd appear" eval "[[ \$(msg link-devices) != *error* ]]"
  "$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
  wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
  step "daemon up: devices=[$(msg link-devices)]"

  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  [[ $(msg link-pair) == ok ]] || fail "link-pair: $(msg link-pair)"
  wait_for "no pairing window in the shell" eval "[[ \$(msg link-pairing) == open\ * ]]"
  read -r _ CODE _ <<< "$(msg link-pairing)"
  phone pair --code "$CODE" --addr "127.0.0.1:$PORT" >> "$OUT/phone.log" || fail "pairing"
  wait_for "shell did not see PairingFinished" eval "[[ \$(msg link-pairing) == \"paired E2E Phone\" ]]"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) =~ ^[0-9a-f]{32}\ disconnected\ E2E\ Phone$ ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  wait_for "no last_seen in DeviceInfo" eval "prop DeviceInfo | grep -q \"last_seen\""
  step "paired: devices=[$(msg link-devices)] info=$(prop DeviceInfo)"
  shot bar-last-seen
  msg panel-open phone > /dev/null
  shot popover-last-seen
  msg panel-close > /dev/null

  dbus-monitor --session "type=method_call,interface=org.freedesktop.Notifications,member=Notify" > "$OUT/notify.txt" 2>&1 &
  wait_for "dbus-monitor did not start" test -s "$OUT/notify.txt"
  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  RUST_LOG=info "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "E2E Phone" \
    hold --status "42,1,wifi,,,media=60,ringer=vibrate,dnd=0" < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  hold=$!
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected E2E Phone\" ]]"
  wait_for "no status on D-Bus" eval "prop DeviceStatus | grep -qF \"(uint32 42, true, '"'"'wifi'"'"', -1)\""
  wait_for "no Wi-Fi transport in DeviceInfo" eval "prop DeviceInfo | grep -qF \"'"'"'wifi'"'"'\""
  wait_for "no ringer in DeviceControls" eval "prop DeviceControls | grep -qF \"'"'"'vibrate'"'"'\""
  say "media {\"player\":\"spotify\",\"name\":\"Spotify\",\"state\":\"playing\",\"title\":\"Night Drive\",\"artist\":\"Kavinsky\",\"album\":\"OutRun\",\"length_ms\":240000,\"position_ms\":30000,\"volume\":70,\"can\":[\"play\",\"pause\"]}"
  wait_for "no phone player on MPRIS" eval "playerctl -l 2> /dev/null | grep -qx andromeda_link_$ID"
  say "notify {\"id\":\"n1\",\"app\":\"Messages\",\"title\":\"Ana\",\"text\":\"On my way\"}"
  wait_for "link-session did not post the notification" grep -q "On my way" "$OUT/notify.txt"
  step "present: status=$(prop DeviceStatus) controls=$(prop DeviceControls) info=$(prop DeviceInfo) grants=$(prop Grants)"
  shot bar
  msg panel-open phone > /dev/null
  shot popover
  msg panel-close > /dev/null
  msg panel-open control-center > /dev/null
  shot quick-settings
  msg panel-close > /dev/null
  msg session lock > /dev/null
  sleep 1 # real time: the lock surface maps and paints
  shot lock-screen
  say "notify {\"id\":\"n2\",\"app\":\"Messages\",\"title\":\"Ana\",\"text\":\"Parked outside\"}"
  wait_for "no held notification in NotificationCounts" eval "prop NotificationCounts | grep -qF \"'"'"'$ID'"'"': uint32 2\""
  shot lock-screen-held
  magick "$OUT/lock-screen-held.png" -resize 200% png:- | tesseract - - 2> /dev/null | grep -q "2 phone notifications" \
    || fail "the lock screen does not count the held notification"
  step "screenshots: bar, popover, quick-settings, lock-screen; held while locked: $(prop NotificationCounts)"

  kill "$hold"
  wait "$hold" || true
  wait_for "phone still connected after it stopped" eval "[[ \$(msg link-devices) == \"$ID disconnected E2E Phone\" ]]"
  [[ $(msg link-unpair "$ID") == ok ]] || fail "link-unpair: $(msg link-unpair "$ID")"
  wait_for "phone still listed after unpair" eval "[[ -z \$(msg link-devices) ]]"
  step "unpaired: devices=[$(msg link-devices)]"
  [[ $(msg link-unpair "$ID") == *"no paired device"* ]] || fail "unpairing twice was accepted"

  kill "$linkd"
  wait "$linkd" || true
  wait_for "shell kept the daemon after it left the bus" eval "[[ \$(msg link-devices) == *\"not running\"* ]]"
  step "daemon gone: $(msg link-devices)"
'
cat "$OUT/steps.txt"
for shot in bar-last-seen popover-last-seen bar popover quick-settings lock-screen lock-screen-held; do
  [[ -s $OUT/$shot.png ]] || { echo "FAIL: no $shot.png" >&2; exit 1; }
done
echo "PASS; artifacts: $OUT"
