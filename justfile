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
# Rebuild docs/design/rulebook.html from rulebook.md, deviations.md and rulebook-specimen.html.
design-rulebook:
    python3 docs/design/build_rulebook.py

# Check a mockup or the specimen against the rulebook (R1, R5, R6, R7, component sizes).
design-check file='docs/design/rulebook.html':
    python3 docs/design/check_layout.py {{file}}

design-reference:
    python3 protocol/gen-design-tokens.py protocol/design-tokens.toml /tmp/design_tokens.h docs/design/tokens.css
    google-chrome-stable --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 --window-size=1600,1010 --screenshot="$PWD/docs/design/reference.png" "file://$PWD/docs/design/reference.html"

# Live shell loop: the user session runs shell/build-f7/noctalia; each new build-f7 binary (mold link) restarts the shell and captures the screen to /tmp/andromeda-live/last.png.
dev-live:
    meson configure shell/build-f7 -Dcpp_link_args=-fuse-ld=mold -Dc_link_args=-fuse-ld=mold
    tools/f7.sh noctalia noctalia-calc.so
    mkdir -p ~/.config/systemd/user/noctalia.service.d /tmp/andromeda-live
    printf '[Service]\nEnvironment=ANDROMEDA_DEV_TOKENS=%s/protocol/design-tokens.toml\nExecStart=\nExecStart=%s/shell/build-f7/noctalia\n' "$PWD" "$PWD" > ~/.config/systemd/user/noctalia.service.d/dev-live.conf
    systemctl --user daemon-reload && systemctl --user restart noctalia.service
    # Restart only on a new binary: a source watch restarts the live shell on every half-done agent edit.
    watchexec -p -w shell/build-f7/noctalia --debounce 500ms -- 'systemctl --user restart noctalia.service && sleep 1 && grim /tmp/andromeda-live/last.png'

# Back to the packaged shell.
dev-live-off:
    rm -f ~/.config/systemd/user/noctalia.service.d/dev-live.conf
    systemctl --user daemon-reload && systemctl --user restart noctalia.service

# The session compositor runs compositor/build-live/andromeda from the next login. A compositor cannot restart under a live session: rebuild, then log out and in.
dev-comp:
    [ -d compositor/build-live ] || meson setup compositor/build-live compositor --buildtype=debugoptimized --prefix=/usr -Dcpp_link_args=-fuse-ld=mold -Dc_link_args=-fuse-ld=mold
    ninja -C compositor/build-live andromeda
    # A relink drops the file cap; without it the GPU context is not high priority.
    sudo setcap cap_sys_nice=ep compositor/build-live/andromeda || echo 'run: sudo setcap cap_sys_nice=ep compositor/build-live/andromeda (GPU high priority)'
    mkdir -p ~/.config/systemd/user/andromeda.service.d
    printf '[Service]\nExecStart=\nExecStart=%s/compositor/build-live/andromeda\n' "$PWD" > ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload

# Back to the packaged compositor from the next login. From a TTY after a dev compositor crash: run this, then log in again.
dev-comp-off:
    rm -f ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload

# A test desktop in a window: dev compositor + dev shell with copies of your config. Rerun after a compositor rebuild.
dev-nested:
    tests/dev-nested.sh

# Rulebook lint: no float size literal, no duration easing in shell and libdesk-ui; only new violations fail. `just design-lint --update` rewrites the baseline.
design-lint *args:
    python3 docs/design/design_lint.py {{args}}
