<p align="center">
  <img src="shell/assets/andromeda.svg" width="128" alt="Andromeda logo">
</p>

<h1 align="center">Andromeda</h1>

<p align="center">A calm, glass desktop for Linux, with your phone built in.</p>

---

Andromeda is a complete Wayland desktop. The compositor, shell, apps and phone link are designed together, so every
surface follows one design and feels like one system.

> **Status:** early preview. There are no published packages yet.

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

## Try it

Andromeda runs on Arch Linux today. Ubuntu 26.04 support is in progress.

```sh
git clone https://github.com/sitish-123/andromeda.git
cd andromeda
just build
just dev-nested   # opens a test desktop in a window
```

To install it as your session, run `just package` and install the packages from `pkg/`. Then pick **Andromeda** on
the login screen.

## Phone app

Build the Android app from `link/apps/android` (Android 10 or later). Open the control center, go to Devices, press
Pair, and scan the code. The app is not on Google Play, because it needs clipboard, storage and accessibility access
that Play does not allow.

## Contributing

Run `just --list` to see every build, test and dev command.

## License

MIT. See [LICENSE](LICENSE).

Andromeda began as a fork of the [Noctalia](https://github.com/noctalia-dev) projects (MIT). Their copyright notices
stay in the `LICENSE` file of each forked directory: `compositor/`, `shell/`, `greeter/` and `portal/`.
