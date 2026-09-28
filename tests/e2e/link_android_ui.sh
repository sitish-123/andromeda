#!/usr/bin/env bash
# Screenshots of every screen of the Android app, in the light and the dark theme, on the emulator against a private
# umbriel-linkd (own dbus-daemon, own XDG_STATE_HOME) and a desktop MPRIS test player: Home empty and paired (scrolled,
# and with a photo picked), the picker's Photos, Videos, and Files tabs, code entry, the QR link confirmation and its working orb, the desktop's page, the
# per-app filter, Media, and the five onboarding pages. Proves the screens render and are reachable by their labels;
# the images are the artifact to judge them by. Writes <theme>-<screen>.png, results.json, linkd.log, maestro.log to
# $OUT (default ./artifacts/link-android-ui). Run under flock /tmp/link-emulator.lock.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-android-ui}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
MAESTRO=${MAESTRO:-$HOME/.maestro/bin/maestro}
SERIAL=${ANDROID_SERIAL:-emulator-5554}
APK=${APK:-$ROOT/link/android/app/build/outputs/apk/debug/app-debug.apk}
FLOWS=$ROOT/link/android/maestro
PACKAGE=org.umbriel.link

rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-android-ui.XXXX)
trap 'adb shell cmd uimode night no > /dev/null 2>&1 || true; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
adb() { "$ADB" -s "$SERIAL" "$@"; }
maestro() { "$MAESTRO" --device "$SERIAL" test -e OUT="$OUT" "$@" >> "$OUT/maestro.log" 2>&1 || fail "maestro $*"; }
theme() { adb shell cmd uimode night "$1" > /dev/null; sleep 1; }

[[ $(adb get-state 2>/dev/null) == device ]] || fail "no emulator at $SERIAL"
[[ -s $APK ]] || fail "no APK at $APK"

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
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/bus
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }

XDG_STATE_HOME=$RUNTIME/desktop "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
python3 "$ROOT/tests/e2e/mpris_test_player.py" "$RUNTIME/player-calls.txt" &

adb shell cmd statusbar collapse
adb uninstall "$PACKAGE" > /dev/null 2>&1 || true
adb install "$APK" > /dev/null || fail "installing $APK"
adb shell cmd notification allow_listener "$PACKAGE/$PACKAGE.notifications.MirrorService"
fixture_photos

# Fixture photos for the in-app picker: two distinct PNGs in Pictures, scanned into MediaStore, with the media grants
# and All files access a user would give.
fixture_photos() {
  python3 - "$RUNTIME" <<'PY'
import os, struct, sys, zlib
def png(path, seed):
    rows = b"".join(b"\0" + bytes((x * seed + y) % 256 for x in range(3 * 64)) for y in range(64))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
for name, seed in (("e2e-pick-a.png", 3), ("e2e-pick-b.png", 7)):
    png(os.path.join(sys.argv[1], name), seed)
PY
  adb shell mkdir -p /sdcard/Pictures
  for name in e2e-pick-a.png e2e-pick-b.png; do adb push "$RUNTIME/$name" "/sdcard/Pictures/$name" > /dev/null; done
  adb shell content call --uri content://media --method scan_volume --arg external_primary > /dev/null
  adb shell pm grant "$PACKAGE" android.permission.READ_MEDIA_IMAGES
  adb shell pm grant "$PACKAGE" android.permission.READ_MEDIA_VIDEO
  adb shell appops set "$PACKAGE" MANAGE_EXTERNAL_STORAGE allow
}

pair() {
  local uri
  uri=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
  adb shell am start -W -a android.intent.action.VIEW -d "'$uri'" "$PACKAGE" > /dev/null
  maestro -e THEME="$1" "$FLOWS/ui-pair-link.yaml"
}

for mode in light dark; do
  theme "$([[ $mode == dark ]] && echo yes || echo no)"
  maestro -e THEME="$mode" "$FLOWS/ui-unpaired.yaml"
  pair "$mode"
  maestro -e THEME="$mode" "$FLOWS/ui-paired.yaml"
  maestro "$FLOWS/unpair.yaml"
  mv "$OUT/8-unpaired.png" "$OUT/$mode-unpaired.png"
  # Pairing again from a clean slate asks for notifications afresh.
  adb shell pm revoke "$PACKAGE" android.permission.POST_NOTIFICATIONS 2> /dev/null || true
done

python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
shots = sorted(name for name in os.listdir(out) if name.endswith(".png"))
themes = {theme: [s for s in shots if s.startswith(theme + "-")] for theme in ("light", "dark")}
assert themes["light"] and len(themes["light"]) == len(themes["dark"]), themes
json.dump({"pass": True, "screenshots": themes}, open(os.path.join(out, "results.json"), "w"), indent=2)
print(json.dumps({t: len(s) for t, s in themes.items()}))
PY
echo "PASS; artifacts: $OUT"
