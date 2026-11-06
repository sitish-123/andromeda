#!/usr/bin/env bash
# Installs Andromeda from the latest GitHub release on Arch Linux, Ubuntu 26.04, Debian testing/sid or Kali:
#   curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/install.sh | bash
set -euo pipefail
REPO=https://github.com/sitish-123/andromeda/releases/latest/download
# The release signing key; pacman and apt accept only packages it signed.
KEY=97DD7B3B28BE58AC8787E32D11437B48C42663B5

[[ $(uname -m) == x86_64 ]] || { echo "Andromeda needs an x86_64 PC." >&2; exit 1; }
# shellcheck source=/dev/null
. /etc/os-release

# Adds the signed apt repository at $1 and installs Andromeda from it.
apt_install() {
  sudo install -d -m 755 /etc/apt/keyrings
  curl -fsSL "$REPO/andromeda-release.asc" | sudo tee /etc/apt/keyrings/andromeda.asc > /dev/null
  gpg --show-keys --with-colons /etc/apt/keyrings/andromeda.asc | grep -q "^fpr:*$KEY:" \
    || { echo "The downloaded key is not the Andromeda release key." >&2; exit 1; }
  echo "deb [signed-by=/etc/apt/keyrings/andromeda.asc] $1 ./" | sudo tee /etc/apt/sources.list.d/andromeda.list > /dev/null
  sudo apt-get update
  sudo apt-get install -y andromeda
}

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
    # Under curl | bash, stdin is the script: pacman must read its questions (replace a conflicting package) from the terminal.
    sudo pacman -Sy --needed andromeda < /dev/tty
    ;;
  ubuntu)
    [[ $VERSION_ID == 26.04 ]] || { echo "Andromeda needs Ubuntu 26.04 (this is $VERSION_ID)." >&2; exit 1; }
    apt_install "$REPO"
    ;;
  debian | kali)
    # Debian testing and sid have no VERSION_ID; Debian 13 lacks libglycin-2.
    [[ $ID == kali || -z ${VERSION_ID:-} ]] || { echo "Andromeda needs Debian testing or sid (this is Debian $VERSION_ID)." >&2; exit 1; }
    # scrcpy is in Debian's contrib component, which a default Debian install does not enable.
    sudo apt-get update
    if ! apt-cache policy scrcpy | grep 'Candidate: [0-9]' > /dev/null; then
      if [[ -f /etc/apt/sources.list.d/debian.sources ]]; then
        sudo sed -i '/^Components:/{/contrib/!s/$/ contrib/}' /etc/apt/sources.list.d/debian.sources
      else
        sudo sed -i -E '/^deb(-src)? /{/ contrib/!s/ main( |$)/ main contrib\1/}' /etc/apt/sources.list
      fi
    fi
    apt_install https://github.com/sitish-123/andromeda/releases/download/debian
    ;;
  *)
    echo "Andromeda supports Arch Linux, Ubuntu 26.04, Debian testing/sid and Kali (this is ${PRETTY_NAME:-$ID})." >&2
    exit 1
    ;;
esac

echo "Andromeda is installed. Log out and choose Andromeda on the login screen."
