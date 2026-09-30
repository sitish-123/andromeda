#!/usr/bin/env bash
# Usage: sudo tools/andromeda-switch.sh
# Installs the packages `just package` built last, then makes the desktop's own login the display manager from the next
# boot: removes the VT 2 greeter trial, swaps in the patched Gaze (tools/gaze-patched.sh), opens the password prompt
# beside the face check for sudo and polkit, and switches SDDM to greetd (tools/greeter-switch.sh). Reboot afterwards.
# Undo: sudo tools/greeter-switch.sh sddm; sudo tools/gaze-patched.sh restore.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Before the install: the trial's remove deletes /etc/pam.d/gdm-face, which the greeter package now owns.
[[ -e /usr/local/bin/noctalia-greeter ]] && "$ROOT/tools/greeter-trial.sh" remove

# The newest andromeda package names the version the last build produced; every package of that build shares it.
newest=$(ls -t "$ROOT"/pkg/andromeda-1:*.pkg.tar.zst | head -1)
version=$(basename "$newest" | sed -E 's/^andromeda-1:([^-]+)-.*/\1/')
mapfile -t packages < <(ls "$ROOT"/pkg/*-"1:$version"-*.pkg.tar.zst)
[[ ${#packages[@]} -gt 0 ]] || { echo "no packages for $version in pkg/; run just package" >&2; exit 1; }
pacman -U --needed --noconfirm "${packages[@]}"

"$ROOT/tools/gaze-patched.sh" install
sed -i 's/^\(auth\s\+sufficient\s\+pam_gaze\.so\)$/\1 simultaneous/' /etc/pam.d/polkit-1 /etc/pam.d/sudo
"$ROOT/tools/greeter-switch.sh" greetd
echo "done: reboot to log in through the Ryoku greeter"
