#!/usr/bin/env bash
# Installs Andromeda from the latest GitHub release on Arch Linux or Ubuntu 26.04:
#   curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/install.sh | bash
set -euo pipefail
REPO=https://github.com/sitish-123/andromeda/releases/latest/download
# The release signing key; pacman and apt accept only packages it signed.
KEY=97DD7B3B28BE58AC8787E32D11437B48C42663B5

[[ $(uname -m) == x86_64 ]] || { echo "Andromeda needs an x86_64 PC." >&2; exit 1; }
# shellcheck source=/dev/null
. /etc/os-release

case $ID in
  arch | cachyos | endeavouros | manjaro)
    key_file=$(mktemp)
    curl -fsSL "$REPO/andromeda-release.asc" -o "$key_file"
    sudo pacman-key --add "$key_file"
    rm -f "$key_file"
    sudo pacman-key --lsign-key "$KEY"
    if ! grep -q '^\[andromeda\]' /etc/pacman.conf; then
      printf '\n[andromeda]\nServer = %s\n' "$REPO" | sudo tee -a /etc/pacman.conf > /dev/null
    fi
    sudo pacman -Sy --needed andromeda
    ;;
  ubuntu)
    [[ $VERSION_ID == 26.04 ]] || { echo "Andromeda needs Ubuntu 26.04 (this is $VERSION_ID)." >&2; exit 1; }
    sudo install -d -m 755 /etc/apt/keyrings
    curl -fsSL "$REPO/andromeda-release.asc" | sudo tee /etc/apt/keyrings/andromeda.asc > /dev/null
    gpg --show-keys --with-colons /etc/apt/keyrings/andromeda.asc | grep -q "^fpr:*$KEY:" \
      || { echo "The downloaded key is not the Andromeda release key." >&2; exit 1; }
    echo "deb [signed-by=/etc/apt/keyrings/andromeda.asc] $REPO ./" | sudo tee /etc/apt/sources.list.d/andromeda.list > /dev/null
    sudo apt-get update
    sudo apt-get install -y andromeda
    ;;
  *)
    echo "Andromeda supports Arch Linux and Ubuntu 26.04 (this is ${PRETTY_NAME:-$ID})." >&2
    exit 1
    ;;
esac

echo "Andromeda is installed. Log out and choose Andromeda on the login screen."
