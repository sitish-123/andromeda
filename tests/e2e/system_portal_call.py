#!/usr/bin/env python3
"""Calls the PT2 impl portals on the Files picker process directly.
Usage: system_portal_call.py account|wallpaper-preview|wallpaper-direct|launcher <handle> [file]
Prints "RESULT <response> <key=value,...>" once the call returns."""
import sys

from gi.repository import Gio, GLib

NAME, PATH = "org.freedesktop.impl.portal.desktop.andromeda.Files", "/org/freedesktop/portal/desktop"
bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def call(iface, method, args, reply):
    return bus.call_sync(NAME, PATH, iface, method, args, GLib.VariantType(reply) if reply else None,
                         Gio.DBusCallFlags.NONE, 120000, None)


kind, arg = sys.argv[1], sys.argv[2]
if kind == "account":
    r = call("org.freedesktop.impl.portal.Account", "GetUserInformation",
             GLib.Variant("(ossa{sv})", (arg, "org.test.Caller", "", {"reason": GLib.Variant("s", "To sign you in")})),
             "(ua{sv})")
    response, results = r.unpack()
    print(f"RESULT {response} " + ",".join(f"{k}={v}" for k, v in sorted(results.items())), flush=True)
elif kind.startswith("wallpaper"):
    options = {"show-preview": GLib.Variant("b", kind == "wallpaper-preview"),
               "set-on": GLib.Variant("s", "background")}
    r = call("org.freedesktop.impl.portal.Wallpaper", "SetWallpaperURI",
             GLib.Variant("(osssa{sv})", (arg, "org.test.Caller", "", "file://" + sys.argv[3], options)), "(u)")
    print(f"RESULT {r.unpack()[0]} ", flush=True)
elif kind == "launcher":
    icon = GLib.Variant("(sv)", ("bytes", GLib.Variant("ay", open(sys.argv[3], "rb").read())))
    r = call("org.freedesktop.impl.portal.DynamicLauncher", "PrepareInstall",
             GLib.Variant("(osssva{sv})", (arg, "org.test.Caller", "", "Web Mail", icon,
                                           {"launcher_type": GLib.Variant("u", 2),
                                            "target": GLib.Variant("s", "https://mail.example")})), "(ua{sv})")
    response, results = r.unpack()
    print(f"RESULT {response} name={results.get('name', '')},icon={'icon' in results}", flush=True)
