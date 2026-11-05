#!/usr/bin/env bash
# P7 (docs/apps.md): in an Andromeda session with a clean HOME, the package's own apps are the defaults:
# Files for folders, Viewer for images and PDFs, Player for audio and video. Installs the three app build dirs
# (no rebuild) and session/applications/andromeda-mimeapps.list into a staging prefix, as pkg/PKGBUILD does.
# Fails when `xdg-mime query default` gives another app for a type, or when a default names a .desktop file that
# is not installed. A session that is not Andromeda (XDG_CURRENT_DESKTOP=GNOME) must keep its own defaults.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

for app in files viewer player; do
  meson install -C "$ROOT/apps/$app/build" --destdir "$STAGE" --skip-subprojects --no-rebuild --quiet > /dev/null
done
APPS=$(find "$STAGE" -type d -path '*/share/applications' | head -n 1)
[[ -n $APPS ]] || fail "no applications dir was installed"
install -Dm644 "$ROOT/session/applications/andromeda-mimeapps.list" "$APPS/andromeda-mimeapps.list"
# A distro-wide plain list names another folder app (Exec must resolve to a file: xdg-mime drops builtins): Andromeda's list must win over it only in an Andromeda session.
printf '[Desktop Entry]\nType=Application\nName=Other\nExec=env\nMimeType=inode/directory;\n' > "$APPS/other-files.desktop"
printf '[Default Applications]\ninode/directory=other-files.desktop\n' > "$APPS/mimeapps.list"
mkdir -p "$STAGE/home"

# xdg-mime skips a default whose Exec binary is not on PATH.
query() { # desktop, mime
  env -i PATH="${APPS%/share/applications}/bin:$PATH" HOME="$STAGE/home" XDG_CONFIG_HOME="$STAGE/home/.config" XDG_DATA_HOME="$STAGE/home/.local/share" \
    XDG_CONFIG_DIRS="$STAGE/etc/xdg" XDG_DATA_DIRS="${APPS%/applications}" XDG_CURRENT_DESKTOP="$1" \
    xdg-mime query default "$2"
}

declare -A want=(
  [inode/directory]=dev.andromeda.Files.desktop
  [image/png]=dev.andromeda.Viewer.desktop [image/jpeg]=dev.andromeda.Viewer.desktop
  [application/pdf]=dev.andromeda.Viewer.desktop
  [audio/mpeg]=dev.andromeda.Player.desktop [audio/flac]=dev.andromeda.Player.desktop
  [video/mp4]=dev.andromeda.Player.desktop [video/x-matroska]=dev.andromeda.Player.desktop
)
for mime in "${!want[@]}"; do
  got=$(query andromeda "$mime")
  [[ $got == "${want[$mime]}" ]] || fail "$mime opens with '$got', not ${want[$mime]}"
done
echo "PASS Andromeda defaults for ${#want[@]} types"

# Every default in the list names an installed app that declares that type.
while IFS='=' read -r mime id; do
  [[ -f $APPS/$id ]] || fail "$mime names $id, which the package does not install"
  grep -qE "^MimeType=(.*;)?$(sed 's/[.+]/\\&/g' <<< "$mime");" "$APPS/$id" || fail "$id does not declare $mime"
done < <(grep -v '^[#[]' "$APPS/andromeda-mimeapps.list" | grep .)
echo "PASS every default is installed and declares its type"

# Another desktop does not read andromeda-mimeapps.list: the plain list decides there.
got=$(query X-Other inode/directory)
[[ $got == other-files.desktop ]] || fail "another desktop opens folders with '$got', not the plain list's other-files.desktop"
echo "PASS other desktops keep their own defaults"
