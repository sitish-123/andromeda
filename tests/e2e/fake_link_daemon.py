#!/usr/bin/env python3
"""A minimal org.andromeda.Link1 for tests: Devices lists one connected phone ("e2e-phone", "Pixel E2E");
SendFiles(s device, a(hs) files) prints "SEND <device> <name>:<bytes> ..." and returns a transfer id.
DeviceInfo gives battery 82, wifi and 88 of 256 GB free. RecentPhotos answers 3 photos (time now, now, a day ago)
whose Thumbnail is a solid PNG; SavePhotoTo writes the photo into the folder and prints "SAVE <path> <folder>".
FAKE_LINK_PHOTOS=denied answers RecentPhotos with .Refused("denied")."""
import io
import os
import time

from gi.repository import Gio, GLib
from PIL import Image

XML = """<node><interface name="org.andromeda.Link1">
<method name="SendFiles"><arg type="s" direction="in"/><arg type="a(hs)" direction="in"/><arg type="s" direction="out"/></method>
<method name="RecentPhotos"><arg type="s" direction="in"/><arg type="q" direction="in"/><arg type="a(sst)" direction="out"/></method>
<method name="Thumbnail"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="q" direction="in"/><arg type="ay" direction="out"/></method>
<method name="SavePhotoTo"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<property name="Devices" type="a(ssb)" access="read"/>
<property name="DeviceInfo" type="a{sa{sv}}" access="read"/>
</interface></node>"""

NOW = int(time.time())
PHOTOS = [("/DCIM/Camera/IMG_0003.jpg", "IMG_0003.jpg", NOW), ("/DCIM/Camera/IMG_0002.jpg", "IMG_0002.jpg", NOW - 60),
          ("/DCIM/Camera/IMG_0001.jpg", "IMG_0001.jpg", NOW - 86400)]
COLOURS = {"/DCIM/Camera/IMG_0003.jpg": (220, 60, 40), "/DCIM/Camera/IMG_0002.jpg": (40, 160, 80),
           "/DCIM/Camera/IMG_0001.jpg": (40, 80, 220)}


def image(path, size, fmt):
    out = io.BytesIO()
    Image.new("RGB", (size, size), COLOURS.get(path, (128, 128, 128))).save(out, fmt)
    return out.getvalue()


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "SendFiles":
        device, files = params.unpack()
        fds = invocation.get_message().get_unix_fd_list()
        parts = []
        for handle, name in files:
            fd = fds.get(handle)
            size = len(os.read(fd, 1 << 20))
            os.close(fd)
            parts.append(f"{name}:{size}")
        print(f"SEND {device} {' '.join(parts)}", flush=True)
        invocation.return_value(GLib.Variant("(s)", ("0001",)))
    elif method == "RecentPhotos":
        if os.environ.get("FAKE_LINK_PHOTOS") == "denied":
            invocation.return_dbus_error("org.andromeda.Link1.Refused", "denied")
            return
        _, count = params.unpack()
        invocation.return_value(GLib.Variant("(a(sst))", (PHOTOS[:count],)))
    elif method == "Thumbnail":
        _, photo, size = params.unpack()
        invocation.return_value(GLib.Variant("(ay)", (image(photo, size, "PNG"),)))
    elif method == "SavePhotoTo":
        _, photo, folder = params.unpack()
        name = next((p[1] for p in PHOTOS if p[0] == photo), None)
        if name is None or not os.path.isdir(folder):
            invocation.return_dbus_error("org.andromeda.Link1.Rejected", "not allowed")
            return
        saved = os.path.join(folder, name)
        with open(saved, "wb") as f:
            f.write(image(photo, 64, "JPEG"))
        print(f"SAVE {photo} {folder}", flush=True)
        invocation.return_value(GLib.Variant("(s)", (saved,)))


def on_get(conn, sender, path, iface, prop):
    if prop == "DeviceInfo":
        return GLib.Variant("a{sa{sv}}", {"e2e-phone": {
            "connected": GLib.Variant("b", True), "transport": GLib.Variant("s", "wifi"),
            "battery": GLib.Variant("i", 82), "last_seen": GLib.Variant("x", NOW),
            "storage_free": GLib.Variant("t", 88 * 10**9), "storage_total": GLib.Variant("t", 256 * 10**9)}})
    return GLib.Variant("a(ssb)", [("e2e-phone", "Pixel E2E", True)])


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
    conn.register_object("/org/andromeda/Link1", info, on_call, on_get, None)


def on_name(conn, name):
    print("READY", flush=True)


Gio.bus_own_name(Gio.BusType.SESSION, "org.andromeda.Link1", Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.MainLoop().run()
