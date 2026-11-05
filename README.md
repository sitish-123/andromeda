<p align="center">
  <img src="shell/assets/andromeda.svg" width="128" alt="Andromeda logo">
</p>

<h1 align="center">Andromeda</h1>

<p align="center">A calm, glass desktop for Linux, with your phone built in.</p>

---

Andromeda is a complete Wayland desktop. The compositor, shell, apps and phone link are designed together, so every
surface follows one design and feels like one system.

> **Status:** early preview. Expect rough edges, and keep another desktop installed.

## Highlights

**Glass that stays fast.** Panels, the dock and the bar share one glass material. One slider in Settings takes it from
clear to frosted. The compositor draws it in one pass, so it stays light on the GPU.

**A center island.** The top of the screen shows the time at rest. It grows for music, volume, notifications, calls and
timers, then shrinks back.

**Search everything.** Tap Super to search apps, files, the clipboard and emoji, with a preview of each result.

**Your phone, on the desktop.** Pair an Android phone to share files and the clipboard both ways. You can reply to
notifications, control media, take calls, find your phone, and show its screen in a window.

**One Settings app.** Displays, input, appearance, power, network, sound and users are all in one place. You never need
a config file.

**Apps that match.** Files, Player and Viewer use the same toolkit and design as the desktop.

**Battery that lasts.** Hidden apps can freeze in power saver, the focused app gets priority, and a battery page shows
which apps use energy.

## Install

Andromeda supports Arch Linux and Ubuntu 26.04 (x86_64). Each release publishes signed packages for both.

Run this one command. It detects your system, adds the Andromeda repository and installs the desktop:

```sh
curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/install.sh | bash
```

To set it up by hand instead, follow the steps for your system.

### Arch Linux

Trust the release key, then add the repository to `/etc/pacman.conf`:

```sh
curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/andromeda-release.asc -o andromeda.asc
sudo pacman-key --add andromeda.asc
sudo pacman-key --lsign-key 97DD7B3B28BE58AC8787E32D11437B48C42663B5
```

```ini
[andromeda]
Server = https://github.com/sitish-123/andromeda/releases/latest/download
```

Then install:

```sh
sudo pacman -Sy andromeda
```

### Ubuntu 26.04

```sh
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://github.com/sitish-123/andromeda/releases/latest/download/andromeda-release.asc \
  | sudo tee /etc/apt/keyrings/andromeda.asc > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/andromeda.asc] https://github.com/sitish-123/andromeda/releases/latest/download ./" \
  | sudo tee /etc/apt/sources.list.d/andromeda.list
sudo apt update
sudo apt install andromeda
```

The release key fingerprint is `97DD 7B3B 28BE 58AC 8787  E32D 1143 7B48 C426 63B5`.

### After installing

Log out, choose **Andromeda** on the login screen, and log in. The first-run setup opens.

To update, run `sudo pacman -Syu` on Arch or `sudo apt update && sudo apt upgrade` on Ubuntu.

Optional: `andromeda-power-git` (Arch) or `andromeda-power` (Ubuntu) turns on extra laptop power saving (PCIe ASPM, USB autosuspend). It changes kernel
defaults and can upset some USB devices, so it is not installed by default.

## Build from source

```sh
git clone --recursive https://github.com/sitish-123/andromeda.git
cd andromeda
just build
just dev-nested   # opens a test desktop in a window
```

`just package` builds the Arch packages into `pkg/`. `tools/ubuntu-deb.sh` builds the Ubuntu packages in a container.

## Phone app

Download `andromeda-link.apk` from the [latest release](https://github.com/sitish-123/andromeda/releases/latest) and
install it on your phone (Android 10 or later). Open the control center, go to Devices, press Pair, and scan the code. The app is not on Google Play, because it needs clipboard, storage and accessibility access
that Play does not allow.

## Contributing

Run `just --list` to see every build, test and dev command.

## License

MIT. See [LICENSE](LICENSE).

Andromeda began as a fork of the [Noctalia](https://github.com/noctalia-dev) projects (MIT). Their copyright notices
stay in the `LICENSE` file of each forked directory: `compositor/`, `shell/`, `greeter/` and `portal/`.
