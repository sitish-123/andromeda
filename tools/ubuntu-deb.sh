#!/usr/bin/env bash
# Usage: tools/ubuntu-deb.sh [ubuntu-version]
# Builds the Ubuntu .deb packages (debian/) from the working tree in a clean container, then installs them with apt
# to prove their dependencies resolve. Output in ~/.cache/andromeda-ubuntu/deb-<version>/ (tmpfs /tmp is too small): the debs, deps.log, build.log, install.log.
set -euo pipefail
VERSION=${1:-26.04}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/deb-$VERSION
IMAGE=andromeda-deb:ubuntu-$VERSION
mkdir -p "$OUT"
# The container writes as root; clear the last run through it.
docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" sh -c 'rm -rf /o/*'
mkdir -p "$OUT/src/pkg"
# Tracked and new files without the Link submodule (Link ships its own deb) or pkg/ build output.
git -C "$ROOT" ls-files -co --exclude-standard -z | grep -zv -e '^link/' -e '^pkg/' |
  tar -C "$ROOT" --null -T - -c | tar -x -C "$OUT/src"
cp "$ROOT/pkg/PKGBUILD" "$ROOT/pkg/deb-build.sh" "$OUT/src/pkg/"

docker build -q -t "$IMAGE" - > /dev/null <<DOCKER
FROM ubuntu:$VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends devscripts equivs ca-certificates
DOCKER

# Packages from our apt repo that Ubuntu lacks (tools/deb-xwayland-satellite.sh).
mkdir -p "$OUT/extra"
cp "$(dirname "$OUT")/xwayland-satellite-$VERSION"/*.deb "$OUT/extra/" 2> /dev/null || true
status=0
docker run --rm --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT:/o" -w /o/src "$IMAGE" bash -c '
  apt-get update -qq
  mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" debian/control > /o/deps.log 2>&1 || exit 10
  dpkg-buildpackage -b -us -uc > /o/build.log 2>&1 || exit 11
  # andromeda-link comes from the Link release, not this source; install every other package.
  apt-get install -y -qq $(ls /o/*.deb /o/extra/*.deb 2> /dev/null | grep -v "/andromeda_") > /o/install.log 2>&1 || exit 12
' || status=$?
docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" chown -R "$(id -u):$(id -g)" /o
case $status in
  0) echo "PASS: $(ls "$OUT"/*.deb | wc -l) debs built and installed" ;;
  10) echo "FAIL build-deps: $(tail -n 3 "$OUT/deps.log")" ;;
  11) echo "FAIL build: $(tail -n 3 "$OUT/build.log")" ;;
  12) echo "FAIL install: $(tail -n 3 "$OUT/install.log")" ;;
  *) echo "FAIL: container exit $status" ;;
esac
exit "$status"
