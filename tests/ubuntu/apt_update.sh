#!/usr/bin/env bash
# Usage: tests/ubuntu/apt_update.sh [ubuntu-version]
# The apt update backend against a local apt repo (docs/ubuntu-port.md 3.3): andromeda-update built with feature apt
# runs on a private session bus in an Ubuntu container, and its Updates property must list the repo's upgrade and new
# package under "system", and the held and phase-deferred packages under "held". Needs the build image
# (tools/ubuntu-build.sh). Cargo output: ~/.cache/andromeda-ubuntu/apt-test-<version>/.
set -euo pipefail
VERSION=${1:-26.04}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/apt-test-$VERSION
IMAGE=andromeda-apt-test:ubuntu-$VERSION
mkdir -p "$OUT"
docker build -q -t "$IMAGE" - > /dev/null << EOF
FROM andromeda-build:ubuntu-$VERSION
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends dbus dbus-bin libglib2.0-bin
EOF

# Root in the container: the fixture installs packages and holds one.
docker run --rm -v "$ROOT/system:/system:ro" -v "$OUT:/out" "$IMAGE" bash -c '
set -euo pipefail
trap "chown -R $(stat -c %u:%g /out) /out" EXIT
cargo build -q --locked --release --manifest-path /system/Cargo.toml --target-dir /out/target -p andromeda-update \
  --no-default-features --features apt
mk() { # name version [Depends]
  local d=/tmp/pkg/$1-$2
  mkdir -p "$d/DEBIAN"
  printf "Package: %s\nVersion: %s\nArchitecture: all\nMaintainer: t <t@t>\nDescription: fixture\n" "$1" "$2" > "$d/DEBIAN/control"
  [ -z "${3:-}" ] || echo "Depends: $3" >> "$d/DEBIAN/control"
  dpkg-deb -b "$d" "/tmp/$1_$2_all.deb" > /dev/null
}
for p in fx-up fx-held fx-phased fx-dep; do mk "$p" 1.0; done
dpkg -i /tmp/fx-*_1.0_all.deb > /dev/null
mkdir /repo
for p in fx-up fx-held fx-phased; do mk "$p" 2.0; done
mk fx-dep 2.0 fx-new
mk fx-new 1.0
cp /tmp/*_2.0_all.deb /tmp/fx-new_1.0_all.deb /repo/
cd /repo
dpkg-scanpackages . /dev/null 2> /dev/null |
  awk "{print} /^Package: fx-phased/{p=1} p && /^Version: 2.0/{print \"Phased-Update-Percentage: 0\"; p=0}" > Packages
echo "deb [trusted=yes] file:/repo ./" > /etc/apt/sources.list.d/fx.list
rm -f /etc/apt/sources.list.d/ubuntu.sources
apt-mark hold fx-held > /dev/null

export HOME=/tmp/home XDG_CACHE_HOME=/tmp/home/.cache XDG_STATE_HOME=/tmp/home/.local/state
mkdir -p "$HOME"
dbus-run-session -- bash -c "
  /out/target/release/andromeda-update > /out/service.log 2>&1 &
  call() { gdbus call --session -d org.andromeda.Update1 -o /org/andromeda/Update1 \"\$@\"; }
  for _ in \$(seq 50); do call -m org.freedesktop.DBus.Peer.Ping > /dev/null 2>&1 && break; sleep 0.2; done
  call -m org.andromeda.Update1.Check > /dev/null
  for _ in \$(seq 300); do
    call -m org.freedesktop.DBus.Properties.Get org.andromeda.Update1 Checking | grep -q false && break
    sleep 0.5
  done
  call -m org.freedesktop.DBus.Properties.Get org.andromeda.Update1 Updates > /out/updates.txt
"
'
rows=$(grep -oE "\('[a-z]+', '[a-z-]+'" "$OUT/updates.txt" | tr -d "(',")
expect() {
  grep -qx "$1 $2" <<< "$rows" || { echo "FAIL: no $1 row for $2; Updates: $(cat "$OUT/updates.txt")"; exit 1; }
}
expect system fx-up
expect system fx-dep
expect system fx-new
expect held fx-held
expect held fx-phased
if [ "$(grep -c . <<< "$rows")" -ne 5 ]; then
  echo "FAIL: expected 5 rows; Updates: $(cat "$OUT/updates.txt")"
  exit 1
fi
echo "PASS: apt backend lists 3 system and 2 held rows"
