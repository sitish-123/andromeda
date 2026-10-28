# Installing Andromeda

Andromeda is packaged for several Linux distributions. Prefer a distribution package when one is available; it provides the simplest installation and upgrade path.

> Distribution packages and third-party repositories are maintained by their respective maintainers. Review a repository before installing from it.

## Arch Linux

[`andromeda-git`](https://aur.archlinux.org/packages/andromeda-git) is available in the AUR:

```sh
yay -S andromeda-git
```

## Fedora

[Terra](https://wiki.fyralabs.com/Terra) provides nightly builds:

```sh
sudo dnf install andromeda-nightly
```

## openSUSE

The [Noctalia OBS repository](https://build.opensuse.org/project/show/home\:neifua\:Noctalia) provides [`andromeda-git`](https://build.opensuse.org/package/show/home\:neifua\:Noctalia/andromeda-git).

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

Choose the command matching your distribution:

```sh
sudo apt update

# Debian Trixie
sudo apt install -t trixie-backports andromeda

# Debian Sid and Ubuntu 26.04
sudo apt install andromeda
```

The repository provides `amd64` and `arm64` packages only.

## GNU Guix

Andromeda and its XDG portal are available through the third-party [`midnight`](https://codeberg.org/stampede/midnight) Guix channel. Add the channel to `~/.config/guix/channels.scm`:

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

Run `guix pull`, then install `andromeda` and `xdg-desktop-portal-andromeda`. Adding them to the system configuration makes the Andromeda session available to display managers.

## NixOS

Add Andromeda to your flake inputs:

```nix
{
  inputs.andromeda.url = "github:noctalia-dev/andromeda";
}
```

Import the NixOS module and enable Andromeda in your system configuration:

```nix
{ inputs, ... }:
{
  imports = [ inputs.andromeda.nixosModules.default ];

  programs.andromeda.enable = true;
}
```

The module installs Andromeda, registers its display-manager session, enables the required graphics support, and installs and configures `xdg-desktop-portal-andromeda` for screen capture and sharing. Set `programs.andromeda.portalPackage = null` only when another portal setup should provide those services.

Rebuild the system, then select Andromeda in the display manager or start it from a TTY with `start-andromeda`.

### Home Manager and hjem

The Home Manager module (`inputs.andromeda.homeModules.default`) and the hjem module (`inputs.andromeda.hjemModules.default`) write the user configuration from Nix. `settings` is an attrset serialized to TOML; Home Manager also accepts a raw TOML string or a path:

```nix
{ inputs, ... }:
{
  imports = [ inputs.andromeda.homeModules.default ];

  programs.andromeda = {
    enable = true;
    settings = {
      general.autostart = [ "noctalia" ];
      layout.gap = 5;
      input.keyboard.layout = "de";
      keybinds = {
        "Mod+Return" = "spawn:kitty";
        "Mod+Q" = "window-close";
        "Mod" = "spawn:noctalia msg panel-toggle launcher";
      };
    };
  };
}
```

When `settings` is omitted, both modules leave the user configuration path untouched, so Andromeda loads its packaged configuration.

### Binary cache

Pre-built `x86_64-linux` and `aarch64-linux` packages for Andromeda and its portal are available from [Cachix](https://app.cachix.org/cache/andromeda). Add the cache to the NixOS configuration to avoid compiling them locally:

```nix
{
  nix.settings = {
    extra-substituters = [ "https://andromeda.cachix.org" ];
    extra-trusted-public-keys = [
      "andromeda.cachix.org-1:JfNq/2yg2S6D6z4Z2dVSZrZlDPQTKtexB6GAVLD98nw="
    ];
  };
}
```

Andromeda must use the locked `nixpkgs` input it was built against for its store paths to match the cache. Do not set `inputs.andromeda.inputs.nixpkgs.follows` when using the binary cache.

The [`cachix`](https://github.com/noctalia-dev/andromeda/tree/cachix) branch points to the latest commit that CI cached successfully. Track it instead of `main` to avoid selecting a commit whose binaries are not available yet:

```nix
{
  inputs.andromeda.url = "github:noctalia-dev/andromeda/cachix";
}
```

### Using bundled effects

The module links the bundled effect presets into the system profile, so they are available at a path that stays the same across rebuilds and updates:

```
/run/current-system/sw/share/andromeda/effects
```

Use it in place of `/usr/share/andromeda/effects` when including a preset:

```toml
[include]
files = [
  "/run/current-system/sw/share/andromeda/effects/border/pulse/effect.toml",
]
```

Only the `effects` directory is linked. The example `config.toml` is not, since it is of little use for a declarative setup; it is still available in the package under `share/andromeda`.

If you generate the config from Nix, you can reference the package directly instead, which doesn't depend on the module's link:

```nix
"${config.programs.andromeda.package}/share/andromeda/effects/border/pulse/effect.toml"
```

## Manual build

Manual installations have no automatic upgrade path. Prefer a distribution package when one is available.

AndromedaFX builds as part of the source tree. No separate SceneFX package is needed.

Install a C++23 compiler, Meson, Ninja, `just`, `pkg-config`, `wayland-scanner`, and the development packages listed in [`PACKAGING.md`](https://github.com/noctalia-dev/andromeda/blob/main/PACKAGING.md#dependencies). Then clone, build, and install Andromeda:

```sh
git clone https://github.com/noctalia-dev/andromeda.git
cd andromeda
just configure release /usr/local
just release
sudo meson install -C build-release --no-rebuild
```

The release binary is written to `build-release/andromeda`. `just debug` builds the debug binary at `build-debug/andromeda`.

libxkbcommon 1.10.0 or newer is required at build time and runtime. Its pkg-config name is `xkbcommon`; Andromeda uses its server-side modifier API to preserve keyboard locks and latches.

X11 window shapes require the `xcb-shape` library from libxcb. Debian and Ubuntu need `libxcb-shape0-dev` for building and `libxcb-shape0` at runtime. On openSUSE, `libxcb-devel` provides the build files and `libxcb-shape0` provides the runtime library. Arch, Fedora, Alpine, Nix, Guix, and Void bundle the shape library with libxcb.

`jemalloc` is optional and recommended on glibc. Meson's default `-Djemalloc=auto` uses it when the development package is installed. Use `-Djemalloc=enabled` or `-Djemalloc=disabled` to force the choice. The `auto` setting skips it on non-glibc systems.

The default installation prefix is `/usr/local`. Meson stores the prefix when the build directory is configured. To install under your home directory, configure that prefix explicitly before building, including when reusing an existing build directory:

```sh
just configure release "$HOME/.local"
just release
just install
```

An existing build directory keeps its stored prefix. For a fresh directory, the automatic configure call in `just release` and `just install` does not forward a `prefix=...` override. Use `just configure` to set or change the path, and check the stored `prefix` with `meson configure build-release` before installing. The system-wide example builds as your user and uses elevated permissions only for installation.

For a user-local installation, ensure `$HOME/.local/bin` is on `PATH` in your login profile. On systemd systems, the installed units under `$HOME/.local/lib/systemd/user` also need to be linked into the user manager's search path before starting a native session:

```sh
systemctl --user link \
  "$HOME/.local/lib/systemd/user/andromeda.service" \
  "$HOME/.local/lib/systemd/user/andromeda-session.target" \
  "$HOME/.local/lib/systemd/user/andromeda-shutdown.target"
systemctl --user daemon-reload
```

A user-local install places the session entry under `$HOME/.local/share/wayland-sessions`. Display-manager search paths vary, so the session may need additional display-manager configuration to appear in its menu. You can start it from a TTY with `start-andromeda` after the setup above.

### Building with Nix

Build the package directly:

```sh
nix build
```

The resulting binary is available at `result/bin/andromeda`. For development, enter the project shell and use the Just recipes:

```sh
nix develop
just debug
```

For unit tests, the headless compositor harness, and contributor checks, see [Development Commands](https://github.com/noctalia-dev/andromeda/blob/main/CONTRIBUTING.md#development-commands).

## Starting Andromeda

Installed display-manager sessions start Andromeda through `start-andromeda`. Select Andromeda from your display manager, or start it from a TTY:

```sh
start-andromeda
```

The launcher loads the login profile for supported shells such as bash, zsh, and fish. Environment variables from that profile, including `PATH`, are available to Andromeda and applications started in the session. Interactive shell files such as `~/.zshrc` are not loaded. In a systemd-managed session, the launcher imports the login environment into the user manager before starting Andromeda. Login-profile values take precedence over values with the same names from `environment.d`; variables found only in the user manager remain available. The direct fallback inherits the same login environment without importing it.

In a managed native session, Andromeda places startup, autostart, event, and `spawn:` commands in scopes bound to the compositor service, so they are cleaned up when the session ends. This requires systemd 254 or newer.

The compositor service queues the graphical session targets at startup and reports readiness only after its display and IPC sockets are available and its session environment has been published to systemd and D-Bus. Services ordered after `graphical-session.target`, including desktop portals, wait for that readiness notification. Configured startup commands also wait for the environment import to finish.

Run `andromeda` directly only for a nested development session or an explicitly unmanaged launch.

## Screen sharing

Screen sharing in Discord, browsers, and other portal-aware applications needs [`xdg-desktop-portal-andromeda`](https://github.com/noctalia-dev/xdg-desktop-portal-andromeda).

On rotated or flipped outputs, the shared cursor may appear offset because of a wlroots 0.20.2 cursor metadata issue.

### Picker missing after login

Start the session through `start-andromeda` and install the matching `andromeda.service` alongside the compositor binary. The packaged service uses `Type=notify` to hold the graphical session targets until Andromeda is ready; portal services with `After=graphical-session.target` then inherit the published session environment. No portal service override is required for this startup ordering.

If the picker is still missing, inspect the service and portal logs:

```sh
systemctl --user cat andromeda.service
journalctl --user -b \
  -u xdg-desktop-portal.service \
  -u xdg-desktop-portal-andromeda.service \
  -u xdg-desktop-portal-gtk.service \
  --no-pager
```

A portal that was already running before the session startup transaction can retain an earlier environment. Andromeda does not automatically restart portal services. Nested and unmanaged launches do not provide the packaged service's readiness ordering.

## Logs

Andromeda writes its main log to `$XDG_CACHE_HOME/andromeda/andromeda.log`. If `XDG_CACHE_HOME` is unset, the fallback path is `~/.cache/andromeda/andromeda.log`. The previous file is retained as `andromeda.log.1` when the current log reaches 1 MiB.

Messages from wlroots appear with the `[wlr]` tag at info level and above. To include wlroots debug output when reporting a rendering or display problem, start Andromeda with `ANDROMEDA_WLR_DEBUG=1` set in its environment.

When standard output or standard error is connected to a TTY, raw writes from Andromeda and its child processes are redirected to `$XDG_CACHE_HOME/andromeda/andromeda-stderr.log`, or `~/.cache/andromeda/andromeda-stderr.log` when `XDG_CACHE_HOME` is unset.
