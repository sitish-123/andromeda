#!/usr/bin/env python3
# polkit on a test system bus: CheckAuthorization appends the action id to argv[1] and refuses each action listed in
# the file argv[2], one per line; it allows the rest. It holds the reply to each action listed in the file argv[3]
# (optional) until the action leaves that file, as a pending authentication dialog does. While it owns the name,
# System1 asks it (polkit.rs `skip`).
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

AUTHORITY = "org.freedesktop.PolicyKit1.Authority"


class Authority(dbus.service.Object):
    @dbus.service.method(AUTHORITY, in_signature="(sa{sv})sa{ss}us", out_signature="(bba{ss})",
                         async_callbacks=("reply", "error"))
    def CheckAuthorization(self, subject, action, details, flags, cancellation_id, reply, error):
        with open(sys.argv[1], "a") as log:
            log.write(action + "\n")

        def answer():
            if len(sys.argv) > 3 and action in listed(sys.argv[3]):
                return True
            reply((action not in listed(sys.argv[2]), False, dbus.Dictionary({}, signature="ss")))
            return False

        if answer():
            GLib.timeout_add(50, answer)


def listed(path):
    try:
        with open(path) as f:
            return f.read().split()
    except FileNotFoundError:
        return []


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.PolicyKit1", bus)
Authority(bus, "/org/freedesktop/PolicyKit1/Authority")
print("ready", flush=True)
GLib.MainLoop().run()
