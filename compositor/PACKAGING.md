# Packaging Andromeda

Notes for distribution packagers. End-user installation documentation lives in the [README](README.md) and at [docs.noctalia.dev](https://docs.noctalia.dev/andromeda/).

## Package description

Use this short description for package metadata:

> A Wayland compositor built on wlroots.

## Identity

|                  |                                                                   |
| ---------------- | ----------------------------------------------------------------- |
| Name             | `andromeda`                                                         |
| Homepage         | <https://github.com/noctalia-dev/andromeda>                         |
| Documentation    | <https://docs.noctalia.dev/andromeda/>                              |
| License          | MIT ([LICENSE](LICENSE))                                          |
| Version          | Meson `project(... version: ...)` in [`meson.build`](meson.build) |
| Binary           | `andromeda`                                                         |
| Session launcher | `start-andromeda`                                                   |
| Wayland session  | `andromeda.desktop`                                                 |

Andromeda is Linux-only. The project flake builds `x86_64-linux` and `aarch64-linux` packages.

## Build

Andromeda requires a C++23 compiler and standard library. It uses Meson and Ninja, with `pkg-config` and `wayland-scanner` needed during configuration.

A source tarball or `git archive` of a tag is complete.

Configure, build, and install with the intended final prefix:

```sh
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
meson install -C build
```

The configured data directory is compiled into Andromeda so it can locate the packaged default configuration. Do not configure with one prefix and relocate the installed files to another prefix.

The `tests` feature option defaults to `auto`, which defines unit tests, harness clients, and andromedafx checks only for unsanitized debug builds, so the release build above compiles nothing but the compositor and its installed data. Pass `-Dtests=disabled` to state that intent explicitly, or `-Dtests=enabled` to get the suite in a release build:

```sh
meson setup build --buildtype=release --prefix=/usr -Dtests=enabled
meson compile -C build
meson test -C build
```

`jemalloc` is optional and recommended on glibc. The `jemalloc` Meson feature defaults to `auto`. It is skipped on non-glibc systems.

The `localed` Meson feature defaults to `auto` and links libsystemd when it is found. `-Dlocaled=disabled` builds without it; keyboards with no XKB configuration then use libxkbcommon's defaults instead of the systemd-localed keymap.

## andromedafx

Andromeda's scene graph and GLES2 renderer live in `andromedafx/`, a hard fork of [SceneFX](https://github.com/wlrfx/scenefx). It builds in-tree as a static archive and is not installed, so there is no `scenefx` package to satisfy.

An installed SceneFX package is not a substitute and is never consulted. If a distribution already ships SceneFX, the two are unrelated and can coexist.

The archive is linked as a whole so its internal utility symbols survive distribution-provided LTO and archive member pruning.

## Dependencies

### Build and link dependencies

- wlroots 0.20.1 or newer, and strictly below 0.21, built with session, libinput, and Xwayland support: `andromedafx` compiles against wlroots' private struct layouts
- wayland-server 1.24 or newer, plus the Wayland client library
- wayland-protocols 1.47 or newer
- libxkbcommon 1.10.0 or newer (pkg-config: `xkbcommon`), required for server-side keyboard lock and latch updates
- libinput 1.29 or newer
- libeis 1.3 or newer (provided by libei)
- libudev, required for native `[drm]` GPU exclusion support
- libsystemd 243 or newer, required to read the systemd-localed keymap for keyboards with no XKB configuration
- pixman 0.43 or newer
- libdrm 2.4.129 or newer
- libdisplay-info 0.3 or newer
- Cairo and PangoCairo
- tomlplusplus
- nlohmann-json
- xcb, xcb-icccm, xcb-ewmh, and xcb-shape
- xcb-render, xcb-xfixes, xcb-xtest, and libei 1.3 or newer when the `tests` feature is enabled
- EGL, GLES2, and GBM
- lcms2, optional; without it `andromedafx` rejects client ICC profiles and keeps only its parametric color transforms
- jemalloc on glibc, optional

The canonical dependency declarations are in [`meson.build`](meson.build). Distribution package names vary.

`xcb-shape` comes from libxcb and has no additional minimum-version requirement. Debian and Ubuntu split its build files into `libxcb-shape0-dev` and its runtime library into `libxcb-shape0`. On openSUSE, the existing `libxcb-devel` build package provides `xcb-shape.pc` and the headers; the runtime library is `libxcb-shape0`. Arch, Fedora, Alpine, Nix, Guix, and Void include the shape library in their existing libxcb package.

### Runtime dependencies

| Dependency                                 | Role                                                       |
| ------------------------------------------ | ---------------------------------------------------------- |
| `Xwayland`                                 | X11 application support when `general.xwayland` is enabled |
| `xdg-desktop-portal-andromeda`               | ScreenCast, Screenshot, and InputCapture portal backend    |
| A usable font stack                        | Internal overlays and configuration diagnostics            |
| A Wayland-capable graphics and input stack | DRM or nested compositor operation through wlroots         |

`Xwayland` must be discoverable on `PATH`. It may be omitted when a package or installation deliberately disables Xwayland in the configuration.

For a native launch with a working systemd user manager, `start-andromeda` runs the compositor as `andromeda.service`. Andromeda uses `systemd-run` from that existing systemd installation to place application commands in transient scopes under `app.slice`. Managed sessions require systemd 254 or newer so command arguments can be passed without systemd-side environment expansion. Without a reachable systemd user manager, and for nested launches, Andromeda does not invoke `systemd-run` and uses the direct launch path.

The packaged config contains a `spawn:kitty` keybind as an editable example. Kitty is not an Andromeda runtime dependency and does not need to be forced into the compositor package.

## Installed layout

```text
<prefix>/bin/andromeda
<prefix>/bin/start-andromeda
<prefix>/share/andromeda/config.toml
<prefix>/share/wayland-sessions/andromeda.desktop
<prefix>/lib/systemd/user/andromeda.service
<prefix>/lib/systemd/user/andromeda-session.target
<prefix>/lib/systemd/user/andromeda-shutdown.target
<prefix>/share/andromeda/effects/<kind>/<name>/{shader.glsl,effect.toml}
```

`share/andromeda/config.toml` is required. It is installed directly from [`examples/config.toml`](examples/config.toml) and serves as the default when no user or system configuration exists.

Bundled effect presets install under `share/andromeda/effects/`; users include an `effect.toml` and select its name, and nothing is active until selected.

The desktop entry must launch `start-andromeda`. The generated launcher and `andromeda.service` contain the configured absolute path to the `andromeda` binary. Packages using nonstandard paths must preserve both configured references.

## Configuration lookup

Without `-c`, Andromeda selects the first existing configuration in this order:

1. `$XDG_CONFIG_HOME/andromeda/config.toml`, normally `~/.config/andromeda/config.toml`
2. `andromeda/config.toml` under each directory in `$XDG_CONFIG_DIRS`, normally `/etc/xdg/andromeda/config.toml`
3. `<datadir>/andromeda/config.toml`, compiled from the Meson installation paths
4. Internal defaults when none of those files exist

Andromeda captures these candidate paths at startup and watches them for the session. Creating a higher-priority file or removing the current file re-evaluates the lookup without a restart. A selected file with syntax or validation errors does not fall through to the next candidate; the last valid configuration remains active until the file is corrected or removed.

Andromeda never writes a user configuration automatically. An explicit `-c` path stays fixed and never uses the fallback chain.

Users can copy the packaged starting point with:

```sh
mkdir -p ~/.config/andromeda
cp /usr/share/andromeda/config.toml ~/.config/andromeda/config.toml
```

Adjust `/usr/share` when using a different installation prefix.

## Nix integration

The Nix package installs the default configuration into its own store output:

```text
/nix/store/<hash>-andromeda/share/andromeda/config.toml
```

That exact data directory is compiled into the corresponding binary. The NixOS module installs the package and therefore needs no global config file.

Home Manager and hjem leave the user config absent when `programs.andromeda.settings` is `null`. Providing settings generates `$XDG_CONFIG_HOME/andromeda/config.toml`, which takes priority over the packaged file.

## Session integration

Install `andromeda.desktop` under `share/wayland-sessions` so display managers can discover the session. It invokes `start-andromeda`, which uses the systemd user manager when available and directly executes Andromeda otherwise.

For native launches with a supported account shell listed in `/etc/shells`, `start-andromeda` first enters that shell as a noninteractive login shell. This loads exported values from shell-specific login files such as zsh's `~/.zprofile`, while interactive files such as `~/.zshrc` remain outside the session startup path. The direct fallback inherits that environment after discarding graphical variables owned by the new session.

An external service manager can invoke `start-andromeda` as its service process. That fast path does not enter the login shell and instead preserves the manager-provided environment.

The managed path imports the resulting login environment, including `PATH`, then starts `andromeda.service`. Login-profile values take precedence over variables with the same names generated from `environment.d`, while values present only in the user manager remain available. The service uses `Type=notify` and queues `andromeda-session.target` and `graphical-session.target` in the initial startup transaction, ordered after the compositor. Andromeda waits for publication of its graphical session variables and validated `[environment]` assignments to the systemd user manager before sending `READY=1` and launching configured startup commands. The graphical and IPC sockets are available before readiness is reported. Services ordered after `graphical-session.target` therefore wait for the published environment without requiring portal service overrides. A failed environment import prevents the managed compositor from reporting ready. The imported login-environment values are also copied to traditional D-Bus activation. Arbitrary configured values are not copied there. Imported login-environment and configured values remain in the user manager for its lifetime. User quits in the packaged `andromeda.service` activate `andromeda-shutdown.target` before the compositor exits. The service is ordered before `graphical-session.target`, so session services stop first. If shutdown control is unavailable or the request fails, the compositor terminates directly. The launcher also activates the shutdown target and removes the graphical variables after Andromeda exits.

In a managed native session, Andromeda creates a separate transient scope under `app.slice` for the `-s` startup command and every autostart, event hook, and `spawn:` command. Each scope is part of `andromeda-session.target` and bound to the systemd unit that owns the compositor, normally `andromeda.service`. Each scope is ordered after its owner so the application stops before the compositor. Stopping either one, or an unexpected compositor exit, cleans up the scope, including when session-target activation fails. An application failure or OOM kill does not propagate in the other direction, so it cannot stop `andromeda.service`. `systemd-run` waits for scope creation before it replaces itself with the command. If isolation cannot be confirmed, the command is aborted instead of running inside `andromeda.service`. Direct and nested sessions launch the same commands as ordinary compositor children.

No display manager or desktop shell is required by Andromeda itself. It can be paired with [Noctalia](https://github.com/noctalia-dev/noctalia) for panels, notifications, launching, locking, and other desktop-shell services, [noctalia-greeter](https://github.com/noctalia-dev/noctalia-greeter) as the display manager (using greetd), and [xdg-desktop-portal-andromeda](https://github.com/noctalia-dev/xdg-desktop-portal-andromeda) for portal screen capture and sharing.

## Contact

- Issues: <https://github.com/noctalia-dev/andromeda/issues>
- Discord: <https://discord.noctalia.dev>
