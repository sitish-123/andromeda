#!/usr/bin/env python3
"""Saves KWin's workspace as a PNG through org.kde.KWin.ScreenShot2. KWin must run with
KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1, since this script is not an authorized client. Usage: kwin_screenshot.py OUT"""
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402
from PIL import Image  # noqa: E402

read, write = os.pipe()
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
# The list owns its copy of the write end; dropping it and closing ours leaves KWin's as the last one, so the read
# below ends when KWin closes it.
fds = Gio.UnixFDList.new_from_array([os.dup(write)])
result, _ = bus.call_with_unix_fd_list_sync(
    "org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2", "CaptureWorkspace",
    GLib.Variant("(a{sv}h)", ({}, 0)), GLib.VariantType("(a{sv})"), Gio.DBusCallFlags.NONE, 10000, fds, None)
del fds
os.close(write)
data = b""
while chunk := os.read(read, 1 << 20):
    data += chunk
info = result.unpack()[0]
# QImage formats 4 to 6 (RGB32, ARGB32, ARGB32 premultiplied) are BGRA bytes on a little-endian host; 16 to 18
# (RGBX8888, RGBA8888, RGBA8888 premultiplied) are RGBA bytes.
order = "BGRA" if info["format"] in (4, 5, 6) else "RGBA"
image = Image.frombuffer("RGBA", (info["width"], info["height"]), data, "raw", order, info["stride"], 1)
image.convert("RGB").save(sys.argv[1])
