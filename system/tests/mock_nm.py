#!/usr/bin/env python3
# NetworkManager on a test system bus: the root object's Connectivity, Metered, ActiveConnections and
# ConnectivityCheckUri. GetDevices lists none; GetDeviceByIpIface finds the interfaces of the fake nl80211 file
# ANDROMEDA_SYSTEM_FAKE_WIFI names, in state Disconnected. dsk.test.NM.SetConnectivity(u) changes Connectivity
# (1 none, 2 portal, 4 full).
import os

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

    @dbus.service.method("dsk.test.NM", in_signature="u")
    def SetConnectivity(self, value):
        self.props["Connectivity"] = dbus.UInt32(value)
        self.PropertiesChanged(NM, {"Connectivity": self.props["Connectivity"]}, dbus.Array([], signature="s"))


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName(NM, bus)
nm = NetworkManager(bus)
print("ready", flush=True)
GLib.MainLoop().run()
