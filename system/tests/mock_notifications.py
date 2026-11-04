#!/usr/bin/env python3
"""A notification server for tests: each Notify call is appended to argv[1] as `notify <id> <summary> <actions>`.
dsk.test.Notifications.Invoke(u id, s action) emits ActionInvoked as a click would."""
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NAME = "org.freedesktop.Notifications"


class Notifications(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/Notifications")
        self.log = log
        self.next_id = 1

    def write(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(NAME, in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        note, self.next_id = self.next_id, self.next_id + 1
        self.write(f"notify {note} {summary} {','.join(actions)} {hints.get('x-andromeda-rule', '')}")
        return note

    @dbus.service.method(NAME, in_signature="u")
    def CloseNotification(self, note):
        self.write(f"close {note}")

    @dbus.service.method(NAME, out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body"]

    @dbus.service.signal(NAME, signature="us")
    def ActionInvoked(self, note, action):
        pass

    @dbus.service.method("dsk.test.Notifications", in_signature="us")
    def Invoke(self, note, action):
        self.ActionInvoked(note, action)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(sys.argv[2])
name = dbus.service.BusName(NAME, bus)
server = Notifications(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
