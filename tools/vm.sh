#!/usr/bin/env bash
# Usage: tools/vm.sh reset|start [--headless]|install|ssh [CMD...]
# A clean Arch VM for the out-of-box checks (docs/gaps.md Tier A). The official Arch "basic" image (user and password
# `arch`) is the read-only base; `reset` throws away every change by making a new overlay on it.
set -euo pipefail
DIR=${ANDROMEDA_VM_DIR:-$HOME/.cache/andromeda-vm}
URL=https://geo.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-basic.qcow2
PORT=${ANDROMEDA_VM_SSH_PORT:-2222}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=error)
# The image's fixed password `arch`, given by askpass so no prompt stops a script.
export SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=$DIR/askpass
SSH=(ssh -p "$PORT" "${OPTS[@]}" arch@localhost)

base() {
  mkdir -p "$DIR"
  printf '#!/bin/sh\necho arch\n' > "$DIR/askpass" && chmod +x "$DIR/askpass"
  [[ -e $DIR/base.qcow2 ]] && return
  curl -fL --progress-bar -o "$DIR/base.qcow2.part" "$URL"
  (cd "$DIR" && echo "$(curl -fsSL "$URL.SHA256" | cut -d' ' -f1)  base.qcow2.part" | sha256sum -c --quiet)
  mv "$DIR/base.qcow2.part" "$DIR/base.qcow2"
}

case ${1:-} in
  reset)
    base
    rm -f "$DIR/run.qcow2"
    qemu-img create -q -f qcow2 -b base.qcow2 -F qcow2 "$DIR/run.qcow2"
    echo "clean VM at $DIR/run.qcow2"
    ;;
  start)
    [[ -e $DIR/run.qcow2 ]] || "$0" reset
    display=(-device virtio-vga-gl -display gtk,gl=on)
    [[ ${2:-} == --headless ]] && display=(-display none -vga none -serial file:"$DIR/serial.log")
    exec qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G -machine q35 \
      -drive file="$DIR/run.qcow2",if=virtio -nic user,model=virtio-net-pci,hostfwd=tcp::"$PORT"-:22 \
      -device virtio-tablet-pci "${display[@]}"
    ;;
  install)
    # The newest build of each Andromeda package in pkg/, without the opt-in power policy and the old umbriel names.
    mapfile -t pkgs < <(ls -t "$ROOT"/pkg/*.pkg.tar.zst | awk -F/ '{n=$NF; sub(/-[^-]+-[^-]+-[^-]+\.pkg\.tar\.zst$/, "", n)}
      n !~ /^umbriel|power/ && !seen[n]++')
    scp -P "$PORT" "${OPTS[@]}" "${pkgs[@]}" arch@localhost:/tmp/
    "${SSH[@]}" "sudo pacman -Syu --noconfirm && sudo pacman -U --noconfirm /tmp/*.pkg.tar.zst"
    ;;
  ssh) shift; exec "${SSH[@]}" "$@" ;;
  *) sed -n 2p "$0" >&2; exit 2 ;;
esac
