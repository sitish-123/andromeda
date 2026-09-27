#!/usr/bin/env python3
"""Owns org.freedesktop.NetworkManager on the bus in DBUS_SYSTEM_BUS_ADDRESS with one AP-capable Wi-Fi device, and
appends each activation call to argv[1]."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
BASE = "/org/freedesktop/NetworkManager"
PROPS = "org.freedesktop.DBus.Properties"


class Object(dbus.service.Object):
    def __init__(self, bus, path, props):
        super().__init__(bus, path)
        self.props = props

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.props[interface][name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.props.get(interface, {})


class Profile(Object):
    def __init__(self, bus, path, settings):
        super().__init__(bus, path, {})
        self.settings = settings

    @dbus.service.method(NM + ".Settings.Connection", out_signature="a{sa{sv}}")
    def GetSettings(self):
        return self.settings


class Manager(Object):
    def __init__(self, bus, log):
        super().__init__(bus, BASE, {NM: {"ActiveConnections": dbus.Array([], signature="o")}})
        self.bus, self.log, self.profiles, self.actives, self.activations = bus, log, [], [], 0
        Object(bus, BASE + "/Devices/1", {
            NM + ".Device": {"DeviceType": dbus.UInt32(2)},
            NM + ".Device.Wireless": {"WirelessCapabilities": dbus.UInt32(0x40 | 0x10)},
        })
        settings = self

        class Settings(Object):
            @dbus.service.method(NM + ".Settings", out_signature="ao")
            def ListConnections(self):
                return dbus.Array([p for p, _ in settings.profiles], signature="o")

        Settings(bus, BASE + "/Settings", {})

    def record(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    def activate(self, profile):
        self.activations += 1
        path = f"{BASE}/ActiveConnection/{self.activations}"
        self.actives.append(Object(self.bus, path, {NM + ".Connection.Active": {"Connection": dbus.ObjectPath(profile)}}))
        self.props[NM]["ActiveConnections"] = dbus.Array([a.__dbus_object_path__ for a in self.actives], signature="o")
        return dbus.ObjectPath(path)

    @dbus.service.method(NM, out_signature="ao")
    def GetDevices(self):
        return [dbus.ObjectPath(BASE + "/Devices/1")]

    @dbus.service.method(NM, in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, settings, device, specific):
        wireless, ipv4 = settings["802-11-wireless"], settings["ipv4"]
        psk = settings["802-11-wireless-security"]["psk"]
        self.record(f"Add mode={wireless['mode']} ipv4={ipv4['method']} psk_len={len(psk)} device={device}")
        path = f"{BASE}/Settings/{len(self.profiles) + 1}"
        self.profiles.append((dbus.ObjectPath(path), Profile(self.bus, path, settings)))
        return dbus.ObjectPath(path), self.activate(path)

    @dbus.service.method(NM, in_signature="ooo", out_signature="o")
    def ActivateConnection(self, profile, device, specific):
        self.record(f"Activate {profile}")
        return self.activate(profile)

    @dbus.service.method(NM, in_signature="o")
    def DeactivateConnection(self, active):
        self.record(f"Deactivate {active}")
        self.actives = [a for a in self.actives if a.__dbus_object_path__ != active]
        self.props[NM]["ActiveConnections"] = dbus.Array([a.__dbus_object_path__ for a in self.actives], signature="o")


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName(NM, bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
