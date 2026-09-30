#!/usr/bin/env bash
# Usage: sudo tools/greeter-trial.sh install|remove
# Tries the greeter beside SDDM: greetd on VT 2 (Ctrl+Alt+F2), SDDM keeps VT 1 and stays the enabled display manager.
# install copies the release build to /usr/local, the gdm-face PAM stack, and a greetd config for VT 2, then starts
# greetd for this boot only. remove stops greetd and deletes all of it.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
case ${1:-} in
  install)
    pacman -S --needed --noconfirm greetd
    meson install -C "$ROOT/greeter/build-release" --no-rebuild > /dev/null
    install -Dm644 "$ROOT/session/pam.d/gdm-face" /etc/pam.d/gdm-face
    [[ -e /etc/greetd/config.toml.pre-trial ]] || cp /etc/greetd/config.toml /etc/greetd/config.toml.pre-trial
    sed -e 's/^vt = 1$/vt = 2/' -e 's|/usr/bin/noctalia-greeter-session|/usr/local/bin/noctalia-greeter-session|' \
      "$ROOT/session/greetd/config.toml" > /etc/greetd/config.toml
    systemctl start greetd
    echo "greetd is on VT 2: press Ctrl+Alt+F2 to try it, Ctrl+Alt+F1 to come back"
    ;;
  remove)
    systemctl stop greetd || true
    [[ -e /etc/greetd/config.toml.pre-trial ]] && mv /etc/greetd/config.toml.pre-trial /etc/greetd/config.toml
    rm -f /etc/pam.d/gdm-face
    xargs -a "$ROOT/greeter/build-release/meson-logs/install-log.txt" -d '\n' rm -f 2> /dev/null || true
    echo "trial removed; SDDM untouched"
    ;;
  *) echo "usage: sudo $0 install|remove" >&2; exit 2 ;;
esac
