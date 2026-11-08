#!/usr/bin/env python3
"""Calls org.freedesktop.impl.portal.FileChooser on the Files picker directly, so a test can pass options GTK cannot
(choices, no current_folder). Usage: filechooser_impl_call.py SaveFile|OpenFile <app_id> [current_folder]
Prints "RESULT <response> <uris> <choices>" once the window answers."""
import sys

from gi.repository import Gio, GLib

method, app_id = sys.argv[1], sys.argv[2]
options = {
    "choices": GLib.Variant("a(ssa(ss)s)", [
        ("quality", "Quality", [("high", "High"), ("low", "Low")], "low"),
        ("readonly", "Open read-only", [], "true"),
    ]),
}
if len(sys.argv) > 3:
    options["current_folder"] = GLib.Variant("ay", sys.argv[3].encode() + b"\0")
if method == "SaveFile":
    options["current_name"] = GLib.Variant("s", "draft.txt")
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
reply = bus.call_sync("org.freedesktop.impl.portal.desktop.andromeda.Files", "/org/freedesktop/portal/desktop",
                      "org.freedesktop.impl.portal.FileChooser", method,
                      GLib.Variant("(osssa{sv})", ("/org/test/request/1", app_id, "", "", options)),
                      GLib.VariantType("(ua{sv})"), Gio.DBusCallFlags.NONE, 120000, None)
response, results = reply.unpack()
uris = " ".join(results.get("uris", []))
choices = ",".join(f"{k}={v}" for k, v in results.get("choices", []))
print(f"RESULT {response} {uris} {choices}", flush=True)
