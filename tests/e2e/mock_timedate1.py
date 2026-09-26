#!/usr/bin/env python3
"""Owns org.freedesktop.timedate1 on the bus in DBUS_SYSTEM_BUS_ADDRESS. Appends each call to argv[1] and answers
property reads from an in-memory state that SetTimezone/SetNTP/SetLocalRTC/SetTime actually update, emitting
PropertiesChanged so a live TimeDateService client sees the change."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

IFACE = "org.freedesktop.timedate1"


class Manager(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/timedate1")
        self.log = log
        self.state = {
            "Timezone": "UTC",
            "NTP": True,
            "LocalRTC": False,
            "NTPSynchronized": True,
            "TimeUSec": dbus.Int64(1700000000 * 1_000_000),
        }

    def record(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(IFACE, in_signature="sb")
    def SetTimezone(self, timezone, interactive):
        self.record(f"SetTimezone {timezone} {bool(interactive)}")
        self.state["Timezone"] = str(timezone)
        self.PropertiesChanged(IFACE, {"Timezone": self.state["Timezone"]}, [])

    @dbus.service.method(IFACE, in_signature="bb")
    def SetNTP(self, use_ntp, interactive):
        self.record(f"SetNTP {bool(use_ntp)} {bool(interactive)}")
        self.state["NTP"] = bool(use_ntp)
        self.PropertiesChanged(IFACE, {"NTP": self.state["NTP"]}, [])

    @dbus.service.method(IFACE, in_signature="xbb")
    def SetTime(self, usec_utc, relative, interactive):
        self.record(f"SetTime {int(usec_utc)} {bool(relative)} {bool(interactive)}")
        self.state["TimeUSec"] = dbus.Int64(usec_utc)
        self.PropertiesChanged(IFACE, {"TimeUSec": self.state["TimeUSec"]}, [])

    @dbus.service.method(IFACE, in_signature="bbb")
    def SetLocalRTC(self, local_rtc, fix_system, interactive):
        self.record(f"SetLocalRTC {bool(local_rtc)} {bool(fix_system)} {bool(interactive)}")
        self.state["LocalRTC"] = bool(local_rtc)
        self.PropertiesChanged(IFACE, {"LocalRTC": self.state["LocalRTC"]}, [])

    @dbus.service.method(IFACE, out_signature="as")
    def ListTimezones(self):
        self.record("ListTimezones")
        return ["UTC", "Europe/Berlin", "America/New_York"]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.state[name]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.state

    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.timedate1", bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
