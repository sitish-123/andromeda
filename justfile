default: check

build:
    cd compositor && just build
    cd shell && just build
    cd portal && just build
    cd greeter && just build
    git submodule update --init link
    cd link && cargo build

# Link's gate: format, pedantic clippy, the proto tests, and the supply-chain policy.
link:
    cd link && cargo fmt --check
    cd link && cargo clippy --all-targets -- -D warnings
    cd link && cargo test
    cd link && cargo deny check

test: build
    cd compositor && just test
    cd shell && just test

# Phone state in one call: screenshot, UI dump, logcat tail and top activity in /tmp/phone-state/<name>. Honors ANDROID_SERIAL.
phone-state name="now":
    #!/usr/bin/env bash
    set -uo pipefail
    adb() { "${ADB:-$HOME/Android/Sdk/platform-tools/adb}" ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"} "$@"; }
    adb get-state > /dev/null 2>&1 || { echo "FAIL: no phone attached (check adb devices, ANDROID_SERIAL)"; exit 1; }
    d=/tmp/phone-state/{{name}}; mkdir -p "$d"
    exec 9> /tmp/link-emulator.lock; flock 9
    adb exec-out screencap -p > "$d/screenshot.png"
    adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1; adb exec-out cat /sdcard/ui.xml > "$d/ui.xml"
    pid=$(adb shell pidof org.andromeda.link | tr -d '\r')
    adb logcat -d -t 2000 ${pid:+--pid="$pid"} | tail -n 200 > "$d/logcat.txt"
    adb shell dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity' | sed 's/^ *//' > "$d/activity.txt"
    printf '%s\n' "$d/screenshot.png" "$d/ui.xml" "$d/logcat.txt"; echo "top: $(cat "$d/activity.txt")"
    grep -E ' E |FATAL' "$d/logcat.txt" | tail -n 5 | cut -c1-200

# Every E2E script; a failure does not stop the run. Logs and failed.txt go to /tmp/andromeda-e2e.
e2e: build
    meson compile -C compositor/build-debug harness-clients
    cd compositor && just asan
    rm -f /tmp/andromeda-e2e/failed.txt
    just e2e-run tests/e2e/*.sh

# Only the scripts that failed in the last run, without a rebuild.
e2e-failed:
    #!/usr/bin/env bash
    f=/tmp/andromeda-e2e/failed.txt
    [ -s "$f" ] || { echo "no failed scripts"; exit 0; }
    just e2e-run $(sed 's|.*|tests/e2e/&.sh|' "$f")

# One E2E script by name, without a rebuild: `just e2e-one link_app`.
e2e-one name:
    just e2e-run tests/e2e/{{name}}.sh

[private]
e2e-run +scripts:
    #!/usr/bin/env bash
    # failed.txt keeps the failures of the last full run; a pass removes its name, a failure adds it.
    d=/tmp/andromeda-e2e; mkdir -p "$d"; touch "$d/failed.txt"; bad=0
    for t in {{scripts}}; do
      n=$(basename "$t" .sh); case $n in lib|mirror) echo "skip $n: a helper, not a test"; continue ;; esac
      sed -i "/^$n\$/d" "$d/failed.txt"
      if bash "$t" > "$d/$n.log" 2>&1; then echo "pass $n"; else echo "FAIL $n: $d/$n.log"; echo "$n" >> "$d/failed.txt"; bad=1; fi
    done
    if [ -s "$d/failed.txt" ]; then echo "still failing: $(tr '\n' ' ' < "$d/failed.txt")"; fi
    exit $bad

check: test link
    cd compositor && just check
    just e2e

package:
    # -C: a reused build dir keeps old subproject options (default_options apply only on the first configure).
    cd pkg && ANDROMEDA_SOURCE="file://$(dirname "$PWD")#branch=$(git branch --show-current)" makepkg -fC
    # makepkg writes the resolved pkgver back into the PKGBUILD.
    git checkout pkg/PKGBUILD

# The design reference: tokens.css from protocol/design-tokens.toml, then docs/design/reference.png (1600 px wide).
design-reference:
    python3 protocol/gen-design-tokens.py protocol/design-tokens.toml /tmp/design_tokens.h docs/design/tokens.css
    google-chrome-stable --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 --window-size=1600,1010 --screenshot="$PWD/docs/design/reference.png" "file://$PWD/docs/design/reference.html"
