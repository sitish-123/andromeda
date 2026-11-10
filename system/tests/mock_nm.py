#!/usr/bin/env python3
# NetworkManager on a test system bus: the root object's Connectivity, Metered, ActiveConnections and
# ConnectivityCheckUri. GetDevices lists none; GetDeviceByIpIface finds the interfaces of the fake nl80211 file
# ANDROMEDA_SYSTEM_FAKE_WIFI names, in state Disconnected. dsk.test.NM.SetConnectivity(u) changes Connectivity
# (1 none, 2 portal, 4 full). Saved connection Settings/1 is for everyone, Settings/2 only for user "someone-else".
# ActiveConnection/N runs Settings/N; ActiveConnection/3 runs Settings/1. DeactivateConnection appends its argument to
# argv[1] and fails with ConnectionNotActive for ActiveConnection/3.
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
PROPS = "org.freedesktop.DBus.Properties"


class Device(dbus.service.Object):
    """A device in state Disconnected (30): NetworkManager manages it and can activate on it."""

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return {"State": dbus.UInt32(30)}[name]


class SavedConnection(dbus.service.Object):
    def __init__(self, bus, number, permissions):
        super().__init__(bus, f"/org/freedesktop/NetworkManager/Settings/{number}")
        self.permissions = permissions

    @dbus.service.method(f"{NM}.Settings.Connection", out_signature="a{sa{sv}}")
    def GetSettings(self):
        return {"connection": {"id": f"test{len(self.permissions)}", "type": "802-3-ethernet",
                               "permissions": dbus.Array(self.permissions, signature="s")}}


class ActiveConnection(dbus.service.Object):
    def __init__(self, bus, number, profile):
        super().__init__(bus, f"/org/freedesktop/NetworkManager/ActiveConnection/{number}")
        self.profile = profile

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        path = f"/org/freedesktop/NetworkManager/Settings/{self.profile}"
        return {"Connection": dbus.ObjectPath(path)}[name]


class NetworkManager(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager")
        self.props = {
            "Connectivity": dbus.UInt32(4),
            "Metered": dbus.UInt32(4),
            "ActiveConnections": dbus.Array([], signature="o"),
            "ConnectivityCheckUri": "http://nmcheck.test/check",
        }
        self.devices = {}

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.props[name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.props if interface == NM else {}

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.method(NM, out_signature="ao")
    def GetDevices(self):
        return dbus.Array([], signature="o")

    @dbus.service.method(NM, in_signature="s", out_signature="o")
    def GetDeviceByIpIface(self, iface):
        try:
            with open(os.environ.get("ANDROMEDA_SYSTEM_FAKE_WIFI", "/nonexistent")) as f:
                ifindex = next(line.split()[1] for line in f if line.split()[:1] == [iface])
        except (OSError, StopIteration):
            raise dbus.DBusException(f"no device {iface}", name=f"{NM}.UnknownDevice") from None
        path = f"/org/freedesktop/NetworkManager/Devices/{ifindex}"
        if path not in self.devices:
            self.devices[path] = Device(self.connection, path)
        return dbus.ObjectPath(path)

    @dbus.service.method(NM, in_signature="o")
    def DeactivateConnection(self, active):
        with open(sys.argv[1], "a") as log:
            log.write(f"DeactivateConnection {active}\n")
        if active == "/org/freedesktop/NetworkManager/ActiveConnection/3":
            raise dbus.DBusException("not active", name=f"{NM}.ConnectionNotActive")

    @dbus.service.method("dsk.test.NM", in_signature="u")
    def SetConnectivity(self, value):
        self.props["Connectivity"] = dbus.UInt32(value)
        self.PropertiesChanged(NM, {"Connectivity": self.props["Connectivity"]}, dbus.Array([], signature="s"))


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName(NM, bus)
nm = NetworkManager(bus)
saved = [SavedConnection(bus, 1, []), SavedConnection(bus, 2, ["user:someone-else:"])]
active = [ActiveConnection(bus, 1, 1), ActiveConnection(bus, 2, 2), ActiveConnection(bus, 3, 1)]
print("ready", flush=True)
GLib.MainLoop().run()
