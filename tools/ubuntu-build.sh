#!/usr/bin/env bash
# Usage: tools/ubuntu-build.sh [ubuntu-version] [file...]
# Builds the tree from git HEAD in a clean Ubuntu container (default 26.04), the portability proof for testers.
# Extra file arguments are copied over HEAD, so an uncommitted fix can be tried. Logs: ~/.cache/andromeda-ubuntu/build-<version>/ (tmpfs /tmp is too small).
set -euo pipefail
VERSION=${1:-26.04}
shift || true
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/build-$VERSION
IMAGE=andromeda-build:ubuntu-$VERSION
mkdir -p "$OUT"
rm -rf "$OUT/src"
mkdir -p "$OUT/src" "$OUT/ccache" "$OUT/cargo" "$OUT/target"
git -C "$ROOT" archive HEAD | tar -x -C "$OUT/src"
for f in "$@"; do
  mkdir -p "$OUT/src/$(dirname "$f")"
  cp "$ROOT/$f" "$OUT/src/$f"
done

docker build -q -t "$IMAGE" - > /dev/null <<EOF
FROM ubuntu:$VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
  build-essential ccache g++ glslang-tools libvulkan-dev liblcms2-dev meson ninja-build pkgconf cmake git ca-certificates curl python3 just cargo rustc \
  libwayland-dev wayland-protocols libxkbcommon-dev libinput-dev libeis-dev libpixman-1-dev libdrm-dev \
  libdisplay-info-dev libgbm-dev libegl-dev libgles-dev libepoxy-dev libudev-dev libsystemd-dev libseat-dev \
  hwdata libliftoff-dev xwayland libxcb1-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-render0-dev libxcb-shape0-dev \
  libxcb-composite0-dev libxcb-res0-dev libxcb-xfixes0-dev libxcb-dri3-dev libxcb-present-dev libxcb-shm0-dev libxcb-xinput-dev libxcb-errors-dev libxcb-render-util0-dev libjemalloc-dev \
  libcairo2-dev libpango1.0-dev libfreetype-dev libfontconfig-dev libharfbuzz-dev librsvg2-dev libglib2.0-dev \
  libgtk-4-dev libsdbus-c++-dev libpipewire-0.3-dev libwireplumber-0.5-dev libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev libavformat-dev libavutil-dev libcurl4-gnutls-dev libstb-dev libical-dev libjxl-dev \
  libqalculate-dev libqrencode-dev libsecret-1-dev libsodium-dev libwebp-dev libxml2-dev libzip-dev libmd4c-dev \
  libmpv-dev nlohmann-json3-dev libpolkit-agent-1-dev libpolkit-gobject-1-dev libpoppler-glib-dev libsndfile1-dev \
  libsqlite3-dev libtag-dev libtomlplusplus-dev libexiv2-dev libcrypt-dev libglycin-2-dev libpam0g-dev \
  libalpm-dev libdbus-1-dev libssl-dev
EOF

failed=0
run() {
  local name=$1
  shift
  if docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT/src:/src" -w /src \
    -v "$OUT/ccache:/ccache" -e CCACHE_DIR=/ccache -v "$OUT/cargo:/tmp/.cargo" -v "$OUT/target:/target" -e CARGO_TARGET_DIR=/target "$IMAGE" bash -c "$*" > "$OUT/$name.log" 2>&1; then
    echo "PASS $name"
  else
    failed=1
    echo "FAIL $name: $(grep -m1 -E 'ERROR|error:|Dependency .* not found' "$OUT/$name.log" || tail -n1 "$OUT/$name.log")"
  fi
}

run compositor "meson setup --prefix=/usr -Dtests=disabled /tmp/b compositor && ninja -C /tmp/b"
run shell "meson setup --prefix=/usr /tmp/b shell && ninja -C /tmp/b"
run portal "meson setup --prefix=/usr /tmp/b portal && ninja -C /tmp/b"
run greeter "meson setup --prefix=/usr /tmp/b greeter && ninja -C /tmp/b"
for app in files viewer player; do
  run "app-$app" "meson setup --prefix=/usr /tmp/b apps/$app && ninja -C /tmp/b"
done
for proj in andromeda-preview indexer; do
  run "$proj" "meson setup --prefix=/usr /tmp/b $proj && ninja -C /tmp/b"
done
run system "cd system && cargo build --release --offline 2>/dev/null || cargo build --release"
exit "$failed"
