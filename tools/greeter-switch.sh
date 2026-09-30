#!/usr/bin/env bash
# Usage: sudo tools/greeter-switch.sh greetd|sddm
# greetd makes the Ryoku greeter (noctalia-greeter-desktop-git) the display manager from the next boot, keeping SDDM
# installed; sddm switches back. Neither stops the running session: the change takes effect at reboot.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
case ${1:-} in
  greetd)
    [[ -e /usr/share/noctalia-greeter/greetd.toml ]] || { echo "install noctalia-greeter-desktop-git first" >&2; exit 1; }
    [[ -e /etc/greetd/config.toml.pre-andromeda ]] || cp /etc/greetd/config.toml /etc/greetd/config.toml.pre-andromeda
    install -m644 /usr/share/noctalia-greeter/greetd.toml /etc/greetd/config.toml
    systemctl disable sddm
    systemctl enable greetd
    echo "greetd is the display manager from the next boot; 'sudo $0 sddm' undoes it"
    ;;
  sddm)
    systemctl disable greetd
    systemctl enable sddm
    [[ -e /etc/greetd/config.toml.pre-andromeda ]] && mv /etc/greetd/config.toml.pre-andromeda /etc/greetd/config.toml
    echo "SDDM is the display manager from the next boot"
    ;;
  *) echo "usage: sudo $0 greetd|sddm" >&2; exit 2 ;;
esac
