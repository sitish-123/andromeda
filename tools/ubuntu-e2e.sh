#!/usr/bin/env bash
# Usage: tools/ubuntu-e2e.sh [ubuntu-version] [e2e-name...]
# The unit suites and the headless E2E scripts in a clean Ubuntu container (default 26.04), on the working tree
# (docs/ubuntu-port.md 4). No names: every script in tests/e2e. Build dirs stay in the output dir, so a rerun is
# incremental. NOCTALIA (a path inside the container, /src = the copied tree) overrides the shell binary.
# Output in ~/.cache/andromeda-ubuntu/e2e-<version>/src/: build.log, unit.log, e2e-failed.txt, e2e-logs/.
set -euo pipefail
VERSION=${1:-26.04}
shift || true
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/e2e-$VERSION
IMAGE=andromeda-e2e:ubuntu-$VERSION
mkdir -p "$OUT/src"
# Tracked and new files without the Link submodule; rsync keeps the build dirs and the wrap download.
git -C "$ROOT" ls-files -co --exclude-standard -z | grep -zv '^link/' |
  rsync -a --from0 --files-from=- "$ROOT/" "$OUT/src/"

# The build image (tools/ubuntu-build.sh) plus the test-only libraries and the programs the E2E scripts call.
docker image inspect "andromeda-build:ubuntu-$VERSION" > /dev/null 2>&1 || {
  echo "no andromeda-build:ubuntu-$VERSION image: run tools/ubuntu-build.sh $VERSION first" >&2
  exit 2
}
docker build -q -t "$IMAGE" - > /dev/null << EOF
FROM andromeda-build:ubuntu-$VERSION
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
  libei-dev libxcb-xtest0-dev dbus dbus-bin libglib2.0-bin grim wtype jq wl-clipboard foot imagemagick ffmpeg pipewire pipewire-bin wireplumber \
  pulseaudio-utils gstreamer1.0-tools xdotool mpv flatpak libgl1-mesa-dri mesa-vulkan-drivers fonts-noto-core \
  fonts-noto-color-emoji python3-gi python3-dbus python3-pil gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  xkb-data gdb
EOF

names=("$@")
scripts=tests/e2e/*.sh
if [[ ${#names[@]} -gt 0 ]]; then scripts=$(printf 'tests/e2e/%s.sh ' "${names[@]}"); fi
status=0
# The compositor's renderer needs a DRM render node: pass the host GPU and its render group.
gpu=()
[[ -e /dev/dri ]] && gpu=(--device /dev/dri --group-add "$(stat -c %g /dev/dri/renderD128)")
docker run --rm --user "$(id -u):$(id -g)" "${gpu[@]}" -e HOME=/tmp --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT/src:/src" -w /src \
  -e NOCTALIA -e SCRIPTS="$scripts" "$IMAGE" bash -c '
  set -o pipefail
  # A setup that failed halfway leaves a dir without build.ninja; remove it and set up again.
  { [ -f compositor/build-debug/build.ninja ] || { rm -rf compositor/build-debug &&
      meson setup --buildtype=debug -Dtests=enabled compositor/build-debug compositor; }
    [ -f shell/build-debug/build.ninja ] || { rm -rf shell/build-debug && meson setup --buildtype=debug shell/build-debug shell; }
    # -k0: one run shows every compile error, not only the first.
    meson compile -C compositor/build-debug --ninja-args=-k0 && meson compile -C shell/build-debug --ninja-args=-k0
  } > /src/build.log 2>&1 || exit 10
  { meson test -C compositor/build-debug --no-rebuild --print-errorlogs
    meson test -C shell/build-debug --no-rebuild --print-errorlogs
  } > /src/unit.log 2>&1 || unit=1
  d=/tmp/andromeda-e2e; mkdir -p "$d"; : > /src/e2e-failed.txt
  for t in $SCRIPTS; do
    n=$(basename "$t" .sh); case $n in lib|mirror) continue ;; esac
    if bash "$t" > "$d/$n.log" 2>&1; then echo "pass $n"; else echo "FAIL $n"; echo "$n" >> /src/e2e-failed.txt; fi
  done
  mkdir -p /src/e2e-logs && cp "$d"/*.log /src/e2e-logs/ 2> /dev/null
  [ -z "${unit:-}" ] || exit 11
  [ ! -s /src/e2e-failed.txt ] || exit 12
' || status=$?
case $status in
  0) echo "PASS: unit suites and E2E" ;;
  10) echo "FAIL build: $(tail -n 3 "$OUT/src/build.log")" ;;
  11) echo "FAIL unit suites: $OUT/src/unit.log; E2E failures: $(tr '\n' ' ' < "$OUT/src/e2e-failed.txt")" ;;
  12) echo "FAIL E2E: $(tr '\n' ' ' < "$OUT/src/e2e-failed.txt") (logs: $OUT/src/e2e-logs/)" ;;
  *) echo "FAIL: container exit $status" ;;
esac
exit "$status"
