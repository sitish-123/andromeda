#!/usr/bin/env python3
"""Mock UPower (OnBattery) and power-profiles-daemon on DBUS_SYSTEM_BUS_ADDRESS.

Each ActiveProfile write is appended to argv[1]. dsk.test.Power.SetOnBattery(b) flips the power source and emits
PropertiesChanged the way UPower does. MOCK_BATTERY=1 adds a laptop battery (battery_BAT0, charge limit supported) and a keyboard backlight (0-3);
MOCK_HISTORY=0 empties the battery history; dsk.test.Power.SetBattery(d percent, u state) changes it, and EnableChargeThreshold calls are appended to argv[1].
"""
import os
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = "org.freedesktop.DBus.Properties"
UPOWER = "org.freedesktop.UPower"
PROFILES = "org.freedesktop.UPower.PowerProfiles"


class Delayed:
    """Answers every method call after MOCK_DELAY seconds (default 0)."""

    def _message_cb(self, connection, message):
        delay = float(os.environ.get("MOCK_DELAY", "0"))
        if delay <= 0:
            return super()._message_cb(connection, message)
        GLib.timeout_add(int(delay * 1000), lambda: super(Delayed, self)._message_cb(connection, message) and False)


class PropertyObject(Delayed, dbus.service.Object):
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


DEVICE = "org.freedesktop.UPower.Device"
DEVICES = "/org/freedesktop/UPower/devices/"


def device_props(present, percent, state):
    return {
        "Type": dbus.UInt32(2), "PowerSupply": dbus.Boolean(present), "IsPresent": dbus.Boolean(present),
        "NativePath": "BAT0" if present else "", "Vendor": "Mock", "Model": "M1", "Serial": "42",
        "Percentage": dbus.Double(percent), "State": dbus.UInt32(state), "EnergyRate": dbus.Double(7.5),
        "Energy": dbus.Double(40.0), "EnergyFull": dbus.Double(50.0), "EnergyFullDesign": dbus.Double(60.0),
        "TimeToEmpty": dbus.Int64(3600), "TimeToFull": dbus.Int64(0), "WarningLevel": dbus.UInt32(1),
        "Capacity": dbus.Double(83.3), "ChargeCycles": dbus.Int32(120),
        "ChargeThresholdSupported": dbus.Boolean(present), "ChargeThresholdEnabled": dbus.Boolean(False),
        "ChargeStartThreshold": dbus.UInt32(75), "ChargeEndThreshold": dbus.UInt32(80),
        "ChargeThresholdSettingsSupported": dbus.UInt32(3),
    }


class Device(PropertyObject):
    interface = DEVICE

    def __init__(self, bus, path, props, log):
        super().__init__(bus, path, props)
        self.log = log

    @dbus.service.method(DEVICE, in_signature="b")
    def EnableChargeThreshold(self, enabled):
        with open(self.log, "a") as out:
            out.write(f"EnableChargeThreshold={bool(enabled)}\n")
        self.props["ChargeThresholdEnabled"] = dbus.Boolean(enabled)
        self.changed({"ChargeThresholdEnabled": dbus.Boolean(enabled)})

    @dbus.service.method(DEVICE, in_signature="suu", out_signature="a(udu)")
    def GetHistory(self, kind, timespan, resolution):
        # A day like mockup A: discharge to 45 %, charge 06-11, discharge to 50 %, charge from 21. Newest first.
        now = int(time.time())
        points = []
        if os.environ.get("MOCK_HISTORY") == "0":
            return dbus.Array(points, signature="(udu)")
        for i in range(min(int(resolution), 96)):
            t = now - i * (int(timespan) // 96)
            hour = (t % 86400) / 3600
            charging = 6 <= hour < 11 or hour >= 21
            if kind == "rate":
                value = 12.0 if charging else 6.0 + 4.0 * ((i * 7) % 5) / 5
            else:
                value = min(100.0, 45 + (hour - 6) * 11) if 6 <= hour < 11 else (
                    100 - (hour - 11) * 5 if 11 <= hour < 21 else (50 + (hour - 21) * 10 / 3 if hour >= 21 else 60 - hour * 2.5))
            points.append((dbus.UInt32(t), dbus.Double(value), dbus.UInt32(1 if charging else 2)))
        return dbus.Array(points, signature="(udu)")


KBD = "org.freedesktop.UPower.KbdBacklight"


class KbdBacklight(Delayed, dbus.service.Object):
    def __init__(self, bus, path):
        super().__init__(bus, path)
        self.brightness = 1

    @dbus.service.method(KBD, out_signature="i")
    def GetBrightness(self):
        return self.brightness

    @dbus.service.method(KBD, out_signature="i")
    def GetMaxBrightness(self):
        return 3

    @dbus.service.method(KBD, in_signature="i")
    def SetBrightness(self, value):
        self.brightness = int(value)
        self.BrightnessChanged(self.brightness)

    @dbus.service.signal(KBD, signature="i")
    def BrightnessChanged(self, value):
        pass


class UPower(PropertyObject):
    interface = UPOWER

    def __init__(self, bus, path, props, devices, kbd):
        super().__init__(bus, path, props)
        self.devices = devices
        self.kbd = kbd

    @dbus.service.method(UPOWER, out_signature="ao")
    def EnumerateDevices(self):
        return dbus.Array([d for d in self.devices if not d.endswith("DisplayDevice")], signature="o")

    @dbus.service.method(UPOWER, out_signature="ao")
    def EnumerateKbdBacklights(self):
        return dbus.Array(self.kbd, signature="o")

    @dbus.service.method(UPOWER, out_signature="o")
    def GetDisplayDevice(self):
        return dbus.ObjectPath(DEVICES + "DisplayDevice")

    @dbus.service.method("dsk.test.Power", in_signature="du")
    def SetBattery(self, percent, state):
        for device in self.devices.values():
            values = {"Percentage": dbus.Double(percent), "State": dbus.UInt32(state)}
            device.props.update(values)
            device.changed(values)

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
        self.holds = {}
        self.chosen = "balanced"
        self.next_cookie = 1

    def write(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, interface, name, value):
        self.write(f"{name}={value}")
        # As power-profiles-daemon: a profile set drops every hold.
        self.holds.clear()
        self.chosen = str(value)
        super().Set(interface, name, value)

    def follow_holds(self):
        held = set(self.holds.values())
        active = "power-saver" if "power-saver" in held else "performance" if held else self.chosen
        self.props["ActiveProfile"] = dbus.String(active)
        self.changed({"ActiveProfile": self.props["ActiveProfile"]})

    @dbus.service.method(PROFILES, in_signature="sss", out_signature="u")
    def HoldProfile(self, profile, reason, application_id):
        if profile not in ("power-saver", "performance"):
            raise dbus.exceptions.DBusException("only power-saver and performance", name=PROFILES + ".Error")
        cookie, self.next_cookie = self.next_cookie, self.next_cookie + 1
        self.holds[cookie] = str(profile)
        self.write(f"Hold={profile}")
        self.follow_holds()
        return cookie

    @dbus.service.method(PROFILES, in_signature="u")
    def ReleaseProfile(self, cookie):
        self.write(f"Release={self.holds.pop(int(cookie), 'unknown')}")
        self.follow_holds()


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
names = [dbus.service.BusName(UPOWER, bus), dbus.service.BusName(PROFILES, bus)]
battery = os.environ.get("MOCK_BATTERY") == "1"
devices = {DEVICES + "DisplayDevice": Device(bus, DEVICES + "DisplayDevice", device_props(battery, 64.0, 2), sys.argv[1])}
if battery:
    devices[DEVICES + "battery_BAT0"] = Device(bus, DEVICES + "battery_BAT0", device_props(True, 64.0, 2), sys.argv[1])
kbd = [KbdBacklight(bus, "/org/freedesktop/UPower/KbdBacklight/kbd_backlight")] if battery else []
upower = UPower(bus, "/org/freedesktop/UPower", {"OnBattery": dbus.Boolean(False), "DaemonVersion": "mock"}, devices,
                [k._object_path for k in kbd])
profiles = Profiles(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
