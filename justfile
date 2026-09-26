default: check

build:
    cd compositor && just build
    cd shell && just build
    cd portal && just build

test: build
    cd compositor && just test
    cd shell && just test

e2e: build
    meson compile -C compositor/build-debug harness-clients
    for t in tests/e2e/display_mode.sh tests/e2e/displays.sh tests/e2e/idle_commits.sh; do bash "$t" || exit 1; done

check: test
    cd compositor && just check
    just e2e

package:
    cd pkg && makepkg -f
