# Installing Andromeda

Andromeda is packaged for several Linux distributions. Prefer a distribution
package when one is available; it provides the simplest installation and
upgrade path.

> Distribution packages and third-party repositories are maintained by their
> respective maintainers. Review a repository before installing from it.

## Arch Linux

[`andromeda-git`](https://aur.archlinux.org/packages/andromeda-git) is available in
the AUR:

```sh
yay -S andromeda-git
```

## Fedora

[Terra](https://wiki.fyralabs.com/Terra) provides nightly builds:

```sh
sudo dnf install andromeda-nightly
```

## openSUSE

The [Noctalia OBS repository](https://build.opensuse.org/project/show/home:neifua:Noctalia)
provides
[`andromeda-git`](https://build.opensuse.org/package/show/home:neifua:Noctalia/andromeda-git).

#### Tumbleweed
```sh
sudo zypper addrepo --refresh --name Noctalia https://download.opensuse.org/repositories/home:neifua:Noctalia/openSUSE_Tumbleweed/home:neifua:Noctalia.repo
sudo zypper refresh && sudo zypper install andromeda-git
```

#### Slowroll
```sh
sudo zypper addrepo --refresh --name Noctalia https://download.opensuse.org/repositories/home:neifua:Noctalia/openSUSE_Slowroll/home:neifua:Noctalia.repo
sudo zypper refresh && sudo zypper install andromeda-git
```

## Debian and Ubuntu

The NickH APT repository provides Andromeda for Debian-based distributions.

### Install the repository signing key

```sh
wget https://pkg.noctalia.dev/deb/nickh-archive-keyring.deb
sudo dpkg -i nickh-archive-keyring.deb
```

### Add the repository

Choose the source matching your distribution:

```sh
# Debian Trixie
sudo wget -O /etc/apt/sources.list.d/noctalia-trixie.sources \
  https://pkg.noctalia.dev/deb/noctalia-trixie.sources

# Debian Sid
sudo wget -O /etc/apt/sources.list.d/noctalia-unstable.sources \
  https://pkg.noctalia.dev/deb/noctalia-unstable.sources

# Ubuntu 26.04
sudo wget -O /etc/apt/sources.list.d/noctalia-resolute.sources \
  https://pkg.noctalia.dev/deb/noctalia-resolute.sources
```

### Install Andromeda

```sh
sudo apt update
sudo apt install andromeda
```

The repository provides `amd64` and `arm64` packages only.

## GNU Guix

Andromeda and its XDG portal are available through the third-party
[`midnight`](https://codeberg.org/stampede/midnight) Guix channel. Add the
channel to `~/.config/guix/channels.scm`:

```scheme
(channel
  (name 'midnight)
  (url "https://codeberg.org/stampede/midnight.git")
  (branch "main")
  (introduction
    (make-channel-introduction
      "d97d1568954cfcbf543c9fcdfd5771e2b730ae19"
      (openpgp-fingerprint
        "640A 2C3C E948 22D3 394B 40C3 CAFA EECA 00FF 9B1E"))))
```

Run `guix pull`, then install `andromeda` and
`xdg-desktop-portal-andromeda`. Adding them to the system configuration makes the
Andromeda session available to display managers.

## Manual build

Manual installations have no automatic upgrade path. Prefer a distribution
package when one is available.

Install a C++23 compiler, Meson, Ninja, `just`, `pkg-config`,
`wayland-scanner`, and the development packages listed in
[`PACKAGING.md`](https://github.com/noctalia-dev/andromeda/blob/main/PACKAGING.md#dependencies).
Then clone, build, and
install Andromeda:

```sh
git clone https://github.com/noctalia-dev/andromeda.git
cd andromeda
just release
sudo just install
```

The default installation prefix is `/usr/local`. Set `prefix` when building to
install elsewhere:

```sh
just prefix="$HOME/.local" release
just install
```

## Starting Andromeda

Installed display-manager sessions start Andromeda through `start-andromeda`.
Select Andromeda from your display manager, or start it from a TTY:

```sh
start-andromeda
```

The launcher loads the login profile for supported shells such as bash, zsh,
and fish. Environment variables from that profile, including `PATH`, are
available to Andromeda and applications started in the session. Interactive
shell files such as `~/.zshrc` are not loaded. In a systemd-managed session,
the launcher imports the login environment into the user manager before
starting Andromeda. Login-profile values take precedence over values with the
same names from `environment.d`; variables found only in the user manager
remain available. The direct fallback inherits the same login environment
without importing it.

In a managed native session, Andromeda places startup, autostart, event, and
`spawn:` commands in scopes bound to the compositor service, so they are
cleaned up when the session ends. This requires systemd 254 or newer.

Run `andromeda` directly only for a nested development session or an explicitly
unmanaged launch.

## Logs

Andromeda writes its main log to `$XDG_CACHE_HOME/andromeda/andromeda.log`. If
`XDG_CACHE_HOME` is unset, the fallback path is
`~/.cache/andromeda/andromeda.log`. The previous file is retained as
`andromeda.log.1` when the current log reaches 1 MiB.

When standard output or standard error is connected to a TTY, raw writes from
Andromeda and its child processes are redirected to
`$XDG_CACHE_HOME/andromeda/andromeda-stderr.log`, or
`~/.cache/andromeda/andromeda-stderr.log` when `XDG_CACHE_HOME` is unset.
