#!/usr/bin/env bash
# Usage: tools/ubuntu-deb.sh [--clean] [ubuntu-version]
# Builds the Ubuntu .deb packages (debian/) from the working tree in a container, then installs them with apt to
# prove their dependencies resolve. Incremental: the copied tree, the meson build dirs and the cargo target dir stay
# in the output dir, so a rerun compiles only what changed. --clean builds from nothing (CI, releases).
# Output in ~/.cache/andromeda-ubuntu/deb-<version>/ (tmpfs /tmp is too small): the debs, build.log, install.log.
set -euo pipefail
CLEAN=0
if [[ ${1:-} == --clean ]]; then
  CLEAN=1
  shift
fi
VERSION=${1:-26.04}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/deb-$VERSION
IMAGE=andromeda-deb:ubuntu-$VERSION
# ccache lives beside OUT, so --clean keeps it.
CCACHE=$(dirname "$OUT")/ccache-deb-$VERSION
mkdir -p "$OUT" "$CCACHE"
# The container writes as root: clear through it.
if [[ $CLEAN == 1 ]]; then
  docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" sh -c 'rm -rf /o/*'
else
  docker run --rm -v "$OUT:/o" "ubuntu:$VERSION" sh -c 'rm -f /o/*.deb /o/*.ddeb /o/*.buildinfo /o/*.changes'
fi
mkdir -p "$OUT/src/pkg"
# Tracked and new files without the Link submodule (Link ships its own deb) or pkg/ build output. rsync skips
# unchanged files, so their timestamps stay and ninja does not rebuild them.
git -C "$ROOT" ls-files -co --exclude-standard -z | grep -zv -e '^link/' -e '^pkg/' |
  rsync -a --from0 --files-from=- "$ROOT/" "$OUT/src/"
rsync -a "$ROOT/pkg/PKGBUILD" "$ROOT/pkg/deb-build.sh" "$OUT/src/pkg/"

# The build dependencies from debian/control live in an image layer: docker reuses it until control changes.
CTX=$OUT/image
mkdir -p "$CTX"
cp "$ROOT/debian/control" "$CTX/control"
docker build -q -t "$IMAGE" -f - "$CTX" > /dev/null << DOCKER
FROM ubuntu:$VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends devscripts equivs ca-certificates
COPY control /tmp/control
RUN apt-get update -qq && mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" /tmp/control
RUN apt-get install -y -qq --no-install-recommends ccache
DOCKER

# Packages from our apt repo that Ubuntu lacks (tools/deb-xwayland-satellite.sh).
mkdir -p "$OUT/extra"
cp "$(dirname "$OUT")/xwayland-satellite-$VERSION"/*.deb "$OUT/extra/" 2> /dev/null || true
status=0
# -nc: no pre-clean, so debian/build (meson dirs) and system/target (cargo) survive; dh_prep still clears each
# package's staging dir before the install step.
docker run --rm --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT:/o" -v "$CCACHE:/ccache" -e CCACHE_DIR=/ccache -w /o/src "$IMAGE" bash -c '
  # A kept build stamp would make dh skip the build step, and cargo with it.
  rm -f debian/debhelper-build-stamp
  set -o pipefail
  dpkg-buildpackage -b -nc -us -uc 2>&1 | tee /o/build.log || exit 11
  # andromeda-link comes from the Link release, not this source; install every other package.
  apt-get update -qq
  apt-get install -y -qq $(ls /o/*.deb /o/extra/*.deb 2> /dev/null | grep -v "/andromeda_") 2>&1 | tee /o/install.log || exit 12
' || status=$?
docker run --rm -v "$OUT:/o" -v "$CCACHE:/ccache" "ubuntu:$VERSION" chown -R "$(id -u):$(id -g)" /o /ccache
case $status in
  0) echo "PASS: $(ls "$OUT"/*.deb | wc -l) debs built and installed" ;;
  11) echo "FAIL build: $(tail -n 3 "$OUT/build.log")" ;;
  12) echo "FAIL install: $(tail -n 3 "$OUT/install.log")" ;;
  *) echo "FAIL: container exit $status" ;;
esac
exit "$status"
