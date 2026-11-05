#!/usr/bin/env bash
# Installs Andromeda from the latest GitHub release on Arch Linux or Ubuntu 26.04:
#   curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/install.sh | bash
set -euo pipefail
REPO=https://github.com/sitish-123/andromeda/releases/latest/download

[[ $(uname -m) == x86_64 ]] || { echo "Andromeda needs an x86_64 PC." >&2; exit 1; }
# shellcheck source=/dev/null
. /etc/os-release

case $ID in
  arch | cachyos | endeavouros | manjaro)
    if ! grep -q '^\[andromeda\]' /etc/pacman.conf; then
      printf '\n[andromeda]\nSigLevel = Optional TrustAll\nServer = %s\n' "$REPO" | sudo tee -a /etc/pacman.conf > /dev/null
    fi
    sudo pacman -Sy --needed andromeda
    ;;
  ubuntu)
    [[ $VERSION_ID == 26.04 ]] || { echo "Andromeda needs Ubuntu 26.04 (this is $VERSION_ID)." >&2; exit 1; }
    echo "deb [trusted=yes] $REPO ./" | sudo tee /etc/apt/sources.list.d/andromeda.list > /dev/null
    sudo apt-get update
    sudo apt-get install -y andromeda
    ;;
  *)
    echo "Andromeda supports Arch Linux and Ubuntu 26.04 (this is ${PRETTY_NAME:-$ID})." >&2
    exit 1
    ;;
esac

echo "Andromeda is installed. Log out and choose Andromeda on the login screen."
