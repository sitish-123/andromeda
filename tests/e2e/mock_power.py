#!/usr/bin/env python3
"""Mock UPower (OnBattery) and power-profiles-daemon on DBUS_SYSTEM_BUS_ADDRESS.

Each ActiveProfile write is appended to argv[1]. dsk.test.Power.SetOnBattery(b) flips the power source and emits
PropertiesChanged the way UPower does.
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = "org.freedesktop.DBus.Properties"
UPOWER = "org.freedesktop.UPower"
PROFILES = "org.freedesktop.UPower.PowerProfiles"


class PropertyObject(dbus.service.Object):
    interface = ""

    def __init__(self, bus, path, props):
        super().__init__(bus, path)
        self.props = props

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.props[name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.props if interface == self.interface else {}

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, interface, name, value):
        self.props[name] = value
        self.changed({name: value})

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def changed(self, values):
        self.PropertiesChanged(self.interface, values, [])


class UPower(PropertyObject):
    interface = UPOWER

    @dbus.service.method(UPOWER, out_signature="ao")
    def EnumerateDevices(self):
        return dbus.Array([], signature="o")

    @dbus.service.method("dsk.test.Power", in_signature="b")
    def SetOnBattery(self, on_battery):
        self.props["OnBattery"] = dbus.Boolean(on_battery)
        self.changed({"OnBattery": dbus.Boolean(on_battery)})


class Profiles(PropertyObject):
    interface = PROFILES

    def __init__(self, bus, log):
        profiles = dbus.Array(
            [dbus.Dictionary({"Profile": p, "Driver": "mock"}, signature="sv")
             for p in ("power-saver", "balanced", "performance")],
            signature="a{sv}",
        )
        super().__init__(bus, "/org/freedesktop/UPower/PowerProfiles",
                         {"ActiveProfile": dbus.String("balanced"), "Profiles": profiles})
        self.log = log

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, interface, name, value):
        with open(self.log, "a") as out:
            out.write(f"{name}={value}\n")
        super().Set(interface, name, value)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
names = [dbus.service.BusName(UPOWER, bus), dbus.service.BusName(PROFILES, bus)]
upower = UPower(bus, "/org/freedesktop/UPower", {"OnBattery": dbus.Boolean(False), "DaemonVersion": "mock"})
profiles = Profiles(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
