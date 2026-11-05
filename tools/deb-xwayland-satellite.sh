#!/usr/bin/env bash
# Usage: tools/deb-xwayland-satellite.sh [ubuntu-version]
# Builds xwayland-satellite (X11 apps for the compositor; MPL-2.0) as a .deb for our apt repo: Ubuntu does not ship it.
# The tag matches the Arch package. Output: ~/.cache/andromeda-ubuntu/xwayland-satellite-<version>/ (the deb and build.log).
set -euo pipefail
VERSION=${1:-26.04}
TAG=v0.8.3
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/xwayland-satellite-$VERSION
mkdir -p "$OUT"
docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" sh -c 'rm -rf /o/*'
status=0
docker run --rm --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT:/o" "ubuntu:$VERSION" bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ca-certificates git cargo rustc pkgconf clang libclang-dev \
    libxcb1-dev libxcb-cursor-dev > /dev/null
  git clone -q --depth 1 --branch '"$TAG"' https://github.com/Supreeeme/xwayland-satellite /b
  cd /b
  cargo build --release --locked
  d=/pkg
  install -Dm755 target/release/xwayland-satellite $d/usr/bin/xwayland-satellite
  install -Dm644 LICENSE $d/usr/share/doc/xwayland-satellite/copyright
  mkdir -p $d/DEBIAN
  cat > $d/DEBIAN/control <<CONTROL
Package: xwayland-satellite
Version: '"${TAG#v}"'-1andromeda1
Architecture: amd64
Maintainer: sitish <sitish@akintechnologies.com>
Depends: libc6, libgcc-s1, libxcb1, libxcb-cursor0, xwayland
Homepage: https://github.com/Supreeeme/xwayland-satellite
Description: Xwayland outside the Wayland compositor
 Runs X11 applications on the Andromeda compositor.
CONTROL
  dpkg-deb --root-owner-group -b $d /o/xwayland-satellite_'"${TAG#v}"'-1andromeda1_amd64.deb
' > "$OUT/build.log" 2>&1 || status=$?
docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" chown -R "$(id -u):$(id -g)" /o
if [[ $status -eq 0 ]]; then
  echo "PASS: $(ls "$OUT"/*.deb)"
else
  echo "FAIL: $(tail -n 3 "$OUT/build.log")"
fi
exit "$status"
