default: check

build:
    cd compositor && just build
    cd shell && just build
    cd portal && just build
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

e2e: build
    meson compile -C compositor/build-debug harness-clients
    cd compositor && just asan
    for t in tests/e2e/*.sh; do case $t in */lib.sh|*/mirror.sh) continue ;; esac; bash "$t" || exit 1; done

check: test link
    cd compositor && just check
    just e2e

package:
    cd pkg && ANDROMEDA_SOURCE="file://$(dirname "$PWD")" makepkg -f
    # makepkg writes the resolved pkgver back into the PKGBUILD.
    git checkout pkg/PKGBUILD
