#!/usr/bin/env python3
"""Owns org.freedesktop.login1 on the bus in DBUS_SYSTEM_BUS_ADDRESS and appends each power call to argv[1]."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

MANAGER = "org.freedesktop.login1.Manager"


class Manager(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/login1")
        self.log = log

    def record(self, method, interactive):
        with open(self.log, "a") as out:
            out.write(f"{method} {bool(interactive)}\n")

    @dbus.service.method(MANAGER, in_signature="b")
    def Suspend(self, interactive):
        self.record("Suspend", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def Reboot(self, interactive):
        self.record("Reboot", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def PowerOff(self, interactive):
        self.record("PowerOff", interactive)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.login1", bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
