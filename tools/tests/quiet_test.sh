#!/usr/bin/env bash
# Checks the summaries tools/quiet.sh prints for saved build and test logs.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
q=$here/../quiet.sh
fx=$here/quiet-fixtures
fail=0

expect() {
  local name=$1 want=$2 got
  got=$("${@:3}")
  if [ "$got" = "$want" ]; then
    echo "pass $name"
  else
    echo "FAIL $name"
    diff <(echo "$want") <(echo "$got") | sed 's/^/  /'
    fail=1
  fi
}

expect gcc "shell/src/app/application.cpp:42: 'foo' was not declared in this scope
shell/src/app/application.h:17: expected ';' after class definition
shell/src/ui/panel.cpp:88: undefined reference to \`Panel::layout()'" \
  "$q" parse-build "$fx/gcc.log" shell/build-f7

expect rustc "system/src/power.rs:41: cannot find value \`level\` in this scope
system/src/main.rs:12: expected one of \`,\` or \`}\`, found \`;\`" \
  "$q" parse-build "$fx/rustc.log" system

expect meson "design_lint: shell/src/shell/live_activity/island_lane_layout.h:9 S1 float size literal 8.0F; use a token or scale
clipboard_storage_permissions: clipboard_storage_permissions_test: legacy history was not loaded
wake_timer: (no output)
2 passed, 3 failed" \
  "$q" parse-meson "$fx/testlog.json"

expect cargo "pair::tests::rejects_short_code: link/crates/link-core/src/pair.rs:120: assertion \`left == right\` failed
6 passed, 1 failed" \
  "$q" parse-cargo "$fx/cargo-test.log" link

expect rustc-max3 "system/src/a.rs:1: cannot find value \`a\` in this scope
system/src/b.rs:2: cannot find value \`b\` in this scope
system/src/c.rs:3: cannot find value \`c\` in this scope" \
  "$q" parse-build "$fx/rustc3.log" system

expect cargo-doctest "link/crates/link-core/src/lib.rs:4: failed to resolve: use of undeclared crate or module \`foo\`
doctest failed, to rerun pass \`-p link-core --doc\`
test link: 2 passed, 0 failed log=$fx/cargo-doctest.log" \
  "$q" report-cargo link "$fx/cargo-doctest.log" 101 link

# The fake module counts its runs in a file and fails, so a reused result shows as no new run with the same exit code.
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git -C "$src" init -q
echo "int a;" > "$src/a.c"
rm -f /tmp/andromeda-build/fake.last
mkdir -p "$src/build/meson-info"
echo '{"buildtype": "debug"}' > "$src/build/meson-info/intro-buildoptions.json"
fake() {
  QUIET_FAKE="echo run >> '$src/.runs'; [ -e '$src/build/out' ] || install -m 755 /dev/null '$src/build/out'
    echo 'a.c:1:1: error: boom' >&2; exit 1" QUIET_FAKE_SRC=$src "$q" build fake "$@"
}
# step [--force]: the build's output, then its exit code and the run count.
step() { fake "$@"; echo "$? $(wc -l < "$src/.runs")"; }
printf '.runs\nbuild\n' > "$src/.git/info/exclude"

expect cache-miss "a.c:1: boom
build fake: FAILED log=/tmp/andromeda-build/fake.log
1 1" step
expect cache-hit "cached: a.c:1: boom
cached: build fake: FAILED log=/tmp/andromeda-build/fake.log
1 1" step
echo "int b;" >> "$src/a.c"
expect cache-source-changed "a.c:1: boom
build fake: FAILED log=/tmp/andromeda-build/fake.log
1 2" step
expect cache-force "a.c:1: boom
build fake: FAILED log=/tmp/andromeda-build/fake.log
1 3" step --force
echo '{"buildtype": "release"}' > "$src/build/meson-info/intro-buildoptions.json"
expect cache-config-changed "a.c:1: boom
build fake: FAILED log=/tmp/andromeda-build/fake.log
1 4" step
rm "$src/build/out"
expect cache-output-deleted "a.c:1: boom
build fake: FAILED log=/tmp/andromeda-build/fake.log
1 5" step
expect cache-hit-again "cached: a.c:1: boom
cached: build fake: FAILED log=/tmp/andromeda-build/fake.log
1 5" step

exit "$fail"
