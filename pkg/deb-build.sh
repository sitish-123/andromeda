#!/usr/bin/env bash
# Usage (from debian/rules, at the source root): pkg/deb-build.sh build|install
# Ubuntu packages. Install steps come from the PKGBUILD's package_*() functions, so both distros share one install
# list. The build differs from PKGBUILD build(): no Link (it ships its own deb), and the update crates use apt.
set -euo pipefail
root=$PWD
srcdir=$root/debian/build
mkdir -p "$srcdir"
ln -sfn "$root" "$srcdir/desktop"

# The flags of Arch's arch-meson, except the wrap mode (the wraps download). --libdir=lib as on Arch: the fixed
# /usr/lib/andromeda and /usr/lib/noctalia paths and the PKGBUILD install steps expect it, not Debian's multiarch dir.
arch-meson() {
  meson setup --prefix=/usr --libdir=lib --libexecdir=lib --sbindir=bin --buildtype=plain --auto-features=enabled \
    -Db_pie=true "$@"
}

# shellcheck source=PKGBUILD
source "$root/pkg/PKGBUILD"
cd "$srcdir"

case ${1:-} in
  build)
    # As PKGBUILD build(): the patched wlroots and libliftoff wraps.
    arch-meson desktop/compositor build-compositor -Dcpp_std=c++23 -Dtests=disabled -Db_lto=true \
      --force-fallback-for=wlroots-0.20,libliftoff \
      -Dwlroots:libliftoff=enabled -Dlibliftoff:default_library=static -Dlibliftoff:werror=false
    arch-meson desktop/shell build-shell -Dcpp_std=c++23 -Dtests=disabled -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    arch-meson desktop/portal build-portal -Dcpp_std=c++23
    arch-meson desktop/greeter build-greeter -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    arch-meson desktop/indexer build-indexer -Db_lto=true
    arch-meson desktop/andromeda-preview build-preview -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    for app in files viewer player; do
      arch-meson desktop/apps/$app build-$app -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    done
    for dir in compositor shell portal greeter indexer preview files viewer player; do meson compile -C build-$dir; done
    cargo build --locked --release --manifest-path desktop/system/Cargo.toml --workspace \
      --exclude andromeda-update --exclude andromeda-update-helper
    cargo build --locked --release --manifest-path desktop/system/Cargo.toml -p andromeda-update \
      -p andromeda-update-helper --no-default-features --features apt
    ;;
  install)
    declare -A debs=(
      [andromeda]=andromeda
      [andromeda-compositor-git]=andromeda-compositor
      [noctalia-desktop-git]=noctalia-desktop
      [xdg-desktop-portal-andromeda-git]=xdg-desktop-portal-andromeda
      [andromeda-system-git]=andromeda-system
      [andromeda-power-git]=andromeda-power
      [noctalia-greeter-desktop-git]=noctalia-greeter-desktop
    )
    for fn in "${!debs[@]}"; do
      pkgname=${debs[$fn]}
      pkgdir=$root/debian/$pkgname
      mkdir -p "$pkgdir"
      "package_$fn"
      # Debian keeps licences in debian/copyright.
      rm -rf "$pkgdir/usr/share/licenses"
    done
    # Debian PAM stacks: common-* replace Arch's system-login.
    sed -Ei 's/^(account|password|session)\s+include\s+system-login$/@include common-\1/' "$root"/debian/*/etc/pam.d/*
    # The pacman hook's apt equal is the dpkg trigger in debian/andromeda-system.triggers.
    rm -r "$root/debian/andromeda-system/usr/share/libalpm"
    ;;
  *)
    echo "usage: $0 build|install" >&2
    exit 2
    ;;
esac
