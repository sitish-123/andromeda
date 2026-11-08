#!/usr/bin/env python3
"""Calls org.freedesktop.impl.portal.AppChooser or Access on the Files picker process directly.
Usage: appchooser_impl_call.py choose <handle> <choice>... | access <handle>
Prints "RESULT <response> <choice or id=value,...>" once the dialog answers."""
import sys

from gi.repository import Gio, GLib

kind, handle = sys.argv[1], sys.argv[2]
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
if kind == "choose":
    iface, method = "org.freedesktop.impl.portal.AppChooser", "ChooseApplication"
    args = GLib.Variant("(ossasa{sv})", (handle, "org.test.Caller", "", sys.argv[3:],
                                         {"content_type": GLib.Variant("s", "text/plain")}))
else:
    iface, method = "org.freedesktop.impl.portal.Access", "AccessDialog"
    options = {"grant_label": GLib.Variant("s", "Allow"),
               "choices": GLib.Variant("a(ssa(ss)s)", [("remember", "Remember", [], "true")])}
    args = GLib.Variant("(osssssa{sv})", (handle, "org.test.Caller", "", "Use the camera?", "Sub", "Body", options))
reply = bus.call_sync("org.freedesktop.impl.portal.desktop.andromeda.Files", "/org/freedesktop/portal/desktop",
                      iface, method, args, GLib.VariantType("(ua{sv})"), Gio.DBusCallFlags.NONE, 120000, None)
response, results = reply.unpack()
detail = results.get("choice", "") or ",".join(f"{k}={v}" for k, v in results.get("choices", []))
print(f"RESULT {response} {detail}", flush=True)
