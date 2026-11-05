#!/usr/bin/env bash
# Usage: tools/vm.sh [--distro arch|ubuntu] reset|start [--headless|--gl-headless]|install|ssh [CMD...]
# --gl-headless: a virgl GPU with no window, so a session runs (screenshots over ssh with grim).
# A clean VM for the out-of-box checks (docs/gaps.md Tier A, docs/ubuntu-port.md 4). The official cloud image is the
# read-only base; `reset` throws away every change by making a new overlay on it.
# arch: the Arch "basic" image (user and password `arch`). ubuntu: the Ubuntu 26.04 cloud image, given user and
# password `ubuntu` by a cloud-init seed disk.
set -euo pipefail
DISTRO=arch
if [[ ${1:-} == --distro ]]; then
  DISTRO=${2:?--distro needs arch or ubuntu}
  shift 2
fi
case $DISTRO in
  arch)
    URL=https://geo.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-basic.qcow2
    SUMS=$URL.SHA256
    VMUSER=arch
    DEFAULT_DIR=$HOME/.cache/andromeda-vm
    ;;
  ubuntu)
    UBUNTU_VERSION=${ANDROMEDA_VM_UBUNTU:-26.04}
    URL=https://cloud-images.ubuntu.com/releases/$UBUNTU_VERSION/release/ubuntu-$UBUNTU_VERSION-server-cloudimg-amd64.img
    SUMS=$(dirname "$URL")/SHA256SUMS
    VMUSER=ubuntu
    DEFAULT_DIR=$HOME/.cache/andromeda-vm-ubuntu-$UBUNTU_VERSION
    ;;
  *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac
DIR=${ANDROMEDA_VM_DIR:-$DEFAULT_DIR}
PORT=${ANDROMEDA_VM_SSH_PORT:-2222}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=error)
# The fixed password, given by askpass so no prompt stops a script.
export SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=$DIR/askpass
SSH=(ssh -p "$PORT" "${OPTS[@]}" "$VMUSER@localhost")

base() {
  mkdir -p "$DIR"
  printf '#!/bin/sh\necho %s\n' "$VMUSER" > "$DIR/askpass" && chmod +x "$DIR/askpass"
  [[ -e $DIR/base.qcow2 ]] && return
  curl -fL --progress-bar -o "$DIR/base.qcow2.part" "$URL"
  local sum
  sum=$(curl -fsSL "$SUMS" | awk -v f="$(basename "$URL")" '$2 == f || $2 == "*" f || NF == 1 {print $1; exit}')
  (cd "$DIR" && echo "$sum  base.qcow2.part" | sha256sum -c --quiet)
  mv "$DIR/base.qcow2.part" "$DIR/base.qcow2"
}

# NoCloud seed: a FAT disk labelled CIDATA. The cloud image has no password login until cloud-init sets one.
seed() {
  local tmp
  tmp=$(mktemp -d)
  cat > "$tmp/user-data" << 'EOF'
#cloud-config
password: ubuntu
chpasswd: { expire: false }
ssh_pwauth: true
growpart: { mode: auto, devices: ["/"] }
EOF
  echo "instance-id: andromeda-$(date +%s)" > "$tmp/meta-data"
  rm -f "$DIR/seed.img"
  mkfs.vfat -C -n CIDATA "$DIR/seed.img" 2048 > /dev/null
  mcopy -i "$DIR/seed.img" "$tmp/user-data" "$tmp/meta-data" ::
  rm -rf "$tmp"
}

case ${1:-} in
  reset)
    base
    rm -f "$DIR/run.qcow2"
    qemu-img create -q -f qcow2 -b base.qcow2 -F qcow2 "$DIR/run.qcow2"
    if [[ $DISTRO == ubuntu ]]; then
      # The cloud image root is ~3 GB; the session and its dependencies need more.
      qemu-img resize -q "$DIR/run.qcow2" 24G
      seed
    fi
    echo "clean VM at $DIR/run.qcow2"
    ;;
  start)
    [[ -e $DIR/run.qcow2 ]] || "$0" --distro "$DISTRO" reset
    display=(-device virtio-vga-gl -display gtk,gl=on)
    [[ ${2:-} == --headless ]] && display=(-display none -vga none -serial file:"$DIR/serial.log")
    [[ ${2:-} == --gl-headless ]] && display=(-device virtio-vga-gl -display egl-headless -serial file:"$DIR/serial.log")
    extra=()
    [[ $DISTRO == ubuntu ]] && extra=(-drive file="$DIR/seed.img",if=virtio,format=raw,readonly=on)
    exec qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G -machine q35 \
      -drive file="$DIR/run.qcow2",if=virtio "${extra[@]}" \
      -nic user,model=virtio-net-pci,hostfwd=tcp::"$PORT"-:22 \
      -device virtio-tablet-pci "${display[@]}"
    ;;
  install)
    if [[ $DISTRO == ubuntu ]]; then
      # The debs from tools/ubuntu-deb.sh and the extra packages our apt repo carries (tools/deb-xwayland-satellite.sh).
      CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu
      mapfile -t pkgs < <(ls "$CACHE/deb-$UBUNTU_VERSION"/*.deb "$CACHE/xwayland-satellite-$UBUNTU_VERSION"/*.deb)
      # The andromeda metapackage needs andromeda-link from the Link release; leave it out when that deb is absent.
      if ! printf '%s\n' "${pkgs[@]}" | grep -q '/andromeda-link_'; then
        mapfile -t pkgs < <(printf '%s\n' "${pkgs[@]}" | grep -v '/andromeda_')
      fi
      "${SSH[@]}" "rm -rf /tmp/debs && mkdir /tmp/debs"
      scp -P "$PORT" "${OPTS[@]}" "${pkgs[@]}" "$VMUSER@localhost:/tmp/debs/"
      "${SSH[@]}" "sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/debs/*.deb"
      exit
    fi
    # The newest build of each Andromeda package in pkg/, without the opt-in power policy and the old umbriel names.
    mapfile -t pkgs < <(ls -t "$ROOT"/pkg/*.pkg.tar.zst | awk -F/ '{n=$NF; sub(/-[^-]+-[^-]+-[^-]+\.pkg\.tar\.zst$/, "", n)}
      n !~ /^umbriel|power/ && !seen[n]++')
    scp -P "$PORT" "${OPTS[@]}" "${pkgs[@]}" arch@localhost:/tmp/
    "${SSH[@]}" "sudo pacman -Syu --noconfirm && sudo pacman -U --noconfirm /tmp/*.pkg.tar.zst"
    ;;
  ssh) shift; exec "${SSH[@]}" "$@" ;;
  *) sed -n 2p "$0" >&2; exit 2 ;;
esac
