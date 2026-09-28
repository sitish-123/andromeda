# Andromeda

One Wayland desktop, built as a single system: a compositor, a shell, a portal, and Link, which ties your Android phone
to it (files and clipboard both ways, notifications with reply, media, calls, find my phone, the phone's screen in a
window, and its apps in their own windows).

| Directory | Component | From |
|---|---|---|
| `compositor/` | Umbriel: windows, layouts, effects, input, outputs, motion | fork of [noctalia-dev/umbriel](https://github.com/noctalia-dev/umbriel) |
| `shell/` | Noctalia: bar, launcher, lock, notifications, settings, system services | fork of [noctalia-dev/noctalia](https://github.com/noctalia-dev/noctalia) |
| `portal/` | xdg-desktop-portal backend: screen cast, screenshots, settings, shortcuts | fork of xdg-desktop-portal-umbriel |
| `link/` | Link: the desktop daemon, the mirror and apps viewers, and the Android app | this repository |
| `protocol/` | The private compositor/shell protocol and Link's wire format | this repository |
| `session/`, `pkg/` | systemd user units, firewall profile, packaging | this repository |

## Install (Arch Linux)

Add the repository to `/etc/pacman.conf`:

```ini
[andromeda]
SigLevel = Optional TrustAll
Server = https://github.com/sitish-kumar/andromeda/releases/latest/download
```

Then install, log out, and choose **Umbriel** on the login screen:

```sh
sudo pacman -Sy andromeda
```

Optional: `umbriel-power-git` (laptop power policy), `scrcpy` and `android-tools` (phone apps in windows),
`gst-plugin-va` (hardware decoding of the phone's screen). Every dependency comes from the official Arch repositories.
Packages are not signed yet, hence `SigLevel`; they are served over HTTPS from this repository's releases.

Updates arrive with `sudo pacman -Syu`.

## The phone app

Download `andromeda-link.apk` from the [latest release](https://github.com/sitish-kumar/andromeda/releases/latest)
and install it on the phone (Android 10 or later). In the shell, open the control center's Devices tab and press
Pair, then scan the code with the app. It is not on Google Play: it reads the clipboard through the log permission,
browses storage with All files access, and takes input for screen mirroring through an accessibility service, none of
which Play allows.

## Build from source

```sh
just build      # every component, debug
just check      # unit tests, the compositor harness, and the E2E suite
just package    # the Arch packages from this checkout, into pkg/
```

A tag `v*` runs `.github/workflows/release.yml`, which builds the packages and the signed app and publishes them as a
release.

## Documents

| Document | Covers |
|---|---|
| [docs/principles.md](docs/principles.md) | Code rules every change follows |
| [docs/architecture.md](docs/architecture.md) | Components, ownership, data flow, fork strategy |
| [docs/protocol.md](docs/protocol.md) | The private compositor/shell Wayland protocol |
| [docs/native-apis.md](docs/native-apis.md) | Every native API each module calls, with signatures |
| [docs/performance.md](docs/performance.md) | Budgets, baseline, and how to measure |
| [docs/standards.md](docs/standards.md) | Process split, monorepo, contract, boundary, pipeline |
| [docs/power.md](docs/power.md) | Display-on idle power program and measurement |
| [docs/gaps.md](docs/gaps.md) | What is missing, by owner and tier, and the order of work |
| [docs/link-plan.md](docs/link-plan.md) | Link's stages, from the phone pass to phone apps in windows |
| [link/ARCHITECTURE.md](link/ARCHITECTURE.md) | Link's protocol, transports, and Android app |
