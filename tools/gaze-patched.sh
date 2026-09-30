#!/usr/bin/env bash
# Usage: sudo tools/gaze-patched.sh install|restore
# Swaps gaze-bin's gazed and pam_gaze.so for the release build of ~/src/gaze (branch feat/marker-host-race, the
# greeter race in docs/face.md) until that change is upstream. restore reinstalls the stock package; a gaze-bin
# update also restores it.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
BUILD=${GAZE_BUILD:-/home/${SUDO_USER:-sitish}/src/gaze/target/release}
case ${1:-} in
  install)
    install -m755 "$BUILD/gazed" /usr/bin/gazed
    install -m755 "$BUILD/libpam_gaze.so" /usr/lib/security/pam_gaze.so
    systemctl restart gazed
    echo "patched gazed and pam_gaze.so installed"
    ;;
  restore)
    pacman -S --noconfirm gaze-bin
    systemctl restart gazed
    echo "stock gaze-bin restored"
    ;;
  *) echo "usage: sudo $0 install|restore" >&2; exit 2 ;;
esac
