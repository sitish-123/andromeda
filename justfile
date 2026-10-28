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
    cd pkg && ANDROMEDA_SOURCE="file://$(dirname "$PWD")" makepkg -f
    # makepkg writes the resolved pkgver back into the PKGBUILD.
    git checkout pkg/PKGBUILD
