#!/usr/bin/env bash
# Usage: [DEB_VERSION=x.y.z] tools/ubuntu-deb.sh [--clean] [ubuntu-version | debian-suite]
# Builds the Ubuntu or Debian (a suite name, e.g. testing) .deb packages (debian/) from the working tree in a container, then installs them with apt to
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
[[ $VERSION == [0-9]* ]] && BASE=ubuntu:$VERSION || BASE=debian:$VERSION
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/deb-$VERSION
IMAGE=andromeda-deb:${BASE/:/-}
# ccache lives beside OUT, so --clean keeps it.
CCACHE=$(dirname "$OUT")/ccache-deb-$VERSION
# Crate downloads persist across runs, so a build needs the network only for new crates.
CARGO_CACHE=$(dirname "$OUT")/cargo-deb-$VERSION
mkdir -p "$CARGO_CACHE"
mkdir -p "$OUT" "$CCACHE"
# The container writes as root: clear through it.
if [[ $CLEAN == 1 ]]; then
  docker run --rm -v "$OUT:/o" "$BASE" sh -c 'rm -rf /o/*'
else
  docker run --rm -v "$OUT:/o" "$BASE" sh -c 'rm -f /o/*.deb /o/*.ddeb /o/*.buildinfo /o/*.changes'
fi
mkdir -p "$OUT/src/pkg"
# Tracked and new files without the Link submodule (Link ships its own deb) or pkg/ build output. rsync skips
# unchanged files, so their timestamps stay and ninja does not rebuild them.
git -C "$ROOT" ls-files -co --exclude-standard -z | grep -zv -e '^link/' -e '^pkg/' |
  rsync -a --from0 --files-from=- "$ROOT/" "$OUT/src/"
rsync -a "$ROOT/pkg/PKGBUILD" "$ROOT/pkg/deb-build.sh" "$OUT/src/pkg/"
# A release sets DEB_VERSION from its tag, so apt upgrades between releases; other builds keep 0~git0, which sorts
# below every release.
if [[ -n ${DEB_VERSION:-} ]]; then
  sed -i "1s/([^)]*)/($DEB_VERSION)/" "$OUT/src/debian/changelog"
fi

# The build dependencies from debian/control live in an image layer: docker reuses it until control changes.
CTX=$OUT/image
mkdir -p "$CTX"
cp "$ROOT/debian/control" "$CTX/control"
docker build -q -t "$IMAGE" -f - "$CTX" > /dev/null << DOCKER
FROM $BASE
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends devscripts equivs ca-certificates
COPY control /tmp/control
RUN apt-get update -qq && mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" /tmp/control
RUN apt-get install -y -qq --no-install-recommends ccache
DOCKER

# Packages from our apt repo that the distro lacks (tools/deb-xwayland-satellite.sh).
mkdir -p "$OUT/extra"
cp "$(dirname "$OUT")/xwayland-satellite-$VERSION"/*.deb "$OUT/extra/" 2> /dev/null || true
status=0
# -nc: no pre-clean, so debian/build (meson dirs) and system/target (cargo) survive; dh_prep still clears each
# package's staging dir before the install step.
# Host network: on the default bridge, DNS inside the container failed mid-build in three runs.
docker run --rm --network host --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT:/o" -v "$CCACHE:/ccache" -v "$CARGO_CACHE:/cargo" -e CCACHE_DIR=/ccache -e CARGO_HOME=/cargo -e CARGO_NET_RETRY=10 -w /o/src "$IMAGE" bash -c '
  # A kept build stamp would make dh skip the build step, and cargo with it.
  rm -f debian/debhelper-build-stamp
  set -o pipefail
  # Fetch every crate before the build, with retries: a DNS drop mid-build failed two runs.
  for lock in $(find . -name Cargo.lock -not -path "*/target/*" -not -path "./debian/*"); do
    for try in 1 2 3; do cargo fetch --locked --manifest-path "$(dirname "$lock")/Cargo.toml" > /dev/null 2>> /o/fetch.log && break
      [ $try = 3 ] && { echo "cargo fetch failed: $lock" | tee -a /o/build.log; exit 11; }; sleep 5; done
  done
  dpkg-buildpackage -b -nc -us -uc 2>&1 | tee /o/build.log || exit 11
  # andromeda-link comes from the Link release, not this source; install every other package.
  apt-get update -qq
  apt-get install -y -qq $(ls /o/*.deb /o/extra/*.deb 2> /dev/null | grep -v "/andromeda_\|/andromeda-dbgsym_") 2>&1 | tee /o/install.log || exit 12
' || status=$?
docker run --rm -v "$OUT:/o" -v "$CCACHE:/ccache" -v "$CARGO_CACHE:/cargo" "$BASE" chown -R "$(id -u):$(id -g)" /o /ccache /cargo
case $status in
  0) echo "PASS: $(ls "$OUT"/*.deb | wc -l) debs built and installed" ;;
  11) echo "FAIL build: $(tail -n 3 "$OUT/build.log")" ;;
  12) echo "FAIL install: $(tail -n 3 "$OUT/install.log")" ;;
  *) echo "FAIL: container exit $status" ;;
esac
exit "$status"
