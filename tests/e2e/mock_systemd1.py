#!/usr/bin/env python3
"""Owns org.freedesktop.systemd1 on DBUS_SESSION_BUS_ADDRESS as a fake user manager. Serves ListUnits, GetUnit,
Subscribe/Unsubscribe and per-unit GetAll for the Unit and Service/Scope interfaces. The test drives it through
org.andromeda.MockSystemd.Start/Stop, which add or drop a running unit and emit UnitNew/JobRemoved/UnitRemoved
the way systemd does. Appends each manager call to argv[1]."""
import os
import subprocess
import time
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

MANAGER = "org.freedesktop.systemd1.Manager"
UNIT = "org.freedesktop.systemd1.Unit"
SERVICE = "org.freedesktop.systemd1.Service"
SCOPE = "org.freedesktop.systemd1.Scope"
CONTROL = "org.andromeda.MockSystemd"
HOME = os.environ.get("HOME", "/home/test")
NOW_US = int(time.time()) * 1_000_000


def unit_path(name):
    return "/org/freedesktop/systemd1/unit/" + "".join(c if c.isalnum() else "_%02x" % ord(c) for c in name)


# name -> (active state, slice, fragment, file state, preset, started seconds ago)
UNITS = {
    "opencode.service": ("active", "app.slice", f"{HOME}/.config/systemd/user/opencode.service", "enabled", "disabled", 3 * 3600),
    "app-zen\\x2dbrowser@0f1e2d3c.service": ("active", "app.slice", "", "transient", "", 600),
    "pipewire.service": ("active", "session.slice", "/usr/lib/systemd/user/pipewire.service", "enabled", "enabled", 5 * 3600),
    "dbus-:1.2-org.kde.kded6@0.service": ("active", "app-dbus\\x2d:1.2\\x2dorg.kde.kded6.slice", "/usr/lib/systemd/user/dbus-:.service", "static", "", 2 * 3600),
    "app-andromeda-1835-17.scope": ("active", "app.slice", "", "transient", "", 900),
    "stopped.service": ("inactive", "app.slice", "/usr/lib/systemd/user/stopped.service", "disabled", "disabled", 0),
    "socketed.service": ("active", "app.slice", f"{HOME}/.config/systemd/user/socketed.service", "enabled", "disabled", 2 * 3600),
    "default.target": ("active", "", "/usr/lib/systemd/user/default.target", "static", "", 5 * 3600),
}


class Unit(dbus.service.Object):
    def __init__(self, bus, name, state, cgroup=None):
        super().__init__(bus, unit_path(name))
        self.name = name
        self.state = state
        self.cgroup = cgroup

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        active, slice_, fragment, file_state, preset, ago = self.state
        if interface == UNIT:
            return {
                "Id": self.name,
                "Description": self.name.split(".")[0],
                "ActiveState": active,
                "SubState": "running" if active == "active" else "dead",
                "FragmentPath": fragment,
                "UnitFileState": file_state,
                "UnitFilePreset": preset,
                "ActiveEnterTimestamp": dbus.UInt64(NOW_US - ago * 1_000_000 if active == "active" else 0),
                "TriggeredBy": dbus.Array(["socketed.socket"] if self.name == "socketed.service" else [], signature="s"),
            }
        if interface in (SERVICE, SCOPE):
            cgroup = self.cgroup or f"/user.slice/user-1000.slice/user@1000.service/{slice_}/{self.name}"
            return {"Slice": slice_, "ControlGroup": cgroup, "BusName": ""}
        raise dbus.exceptions.DBusException("org.freedesktop.DBus.Error.UnknownInterface", interface)


class Manager(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/systemd1")
        self.bus = bus
        self.log = log
        self.mirrored = {}  # unit name -> real XDG_RUNTIME_DIR, for units started with StartWithCgroup
        self.units = {name: Unit(bus, name, state) for name, state in UNITS.items()}
        self.job = 0

    def record(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(MANAGER, out_signature="a(ssssssouso)")
    def ListUnits(self):
        self.record("ListUnits")
        return [
            (n, n, "loaded", u.state[0], "running" if u.state[0] == "active" else "dead", "", unit_path(n), dbus.UInt32(0), "", "/")
            for n, u in self.units.items()
        ]

    @dbus.service.method(MANAGER, in_signature="s", out_signature="o")
    def GetUnit(self, name):
        if name not in self.units:
            raise dbus.exceptions.DBusException("org.freedesktop.systemd1.NoSuchUnit", f"Unit {name} not loaded.")
        return unit_path(name)

    @dbus.service.method(MANAGER, in_signature="ss", out_signature="o")
    def StopUnit(self, name, mode):
        self.record(f"StopUnit {name} {mode}")
        unit = self.units.pop(name)
        if name in self.mirrored:
            # A mirrored unit is real: stop it on the real user manager too, so its processes exit.
            subprocess.run(["systemctl", "--user", "stop", name], env={**os.environ, "XDG_RUNTIME_DIR": self.mirrored.pop(name)}, check=False)
        unit.remove_from_connection()
        self.finish_job(name)
        self.UnitRemoved(name, unit_path(name))
        return f"/org/freedesktop/systemd1/job/{self.job}"

    @dbus.service.method(MANAGER, in_signature="ssi")
    def KillUnit(self, name, whom, signal):
        self.record(f"KillUnit {name} {whom} {signal}")
        if name in self.mirrored:
            subprocess.run(["systemctl", "--user", "kill", f"--signal={signal}", name], env={**os.environ, "XDG_RUNTIME_DIR": self.mirrored[name]}, check=False)

    @dbus.service.method(MANAGER, in_signature="s")
    def FreezeUnit(self, name):
        self.record(f"FreezeUnit {name}")
        if name in self.mirrored:
            subprocess.run(["systemctl", "--user", "freeze", name], env={**os.environ, "XDG_RUNTIME_DIR": self.mirrored[name]}, check=False)

    @dbus.service.method(MANAGER, in_signature="s")
    def ThawUnit(self, name):
        self.record(f"ThawUnit {name}")
        if name in self.mirrored:
            subprocess.run(["systemctl", "--user", "thaw", name], env={**os.environ, "XDG_RUNTIME_DIR": self.mirrored[name]}, check=False)

    def set_file_state(self, names, state):
        for n in names:
            if n in self.units:
                u = self.units[n]
                u.state = (u.state[0], u.state[1], u.state[2], state, u.state[4], u.state[5])

    @dbus.service.method(MANAGER, in_signature="asb", out_signature="a(sss)")
    def DisableUnitFiles(self, names, runtime):
        self.record(f"DisableUnitFiles {' '.join(names)} {bool(runtime)}")
        self.set_file_state(names, "disabled")
        return dbus.Array([("unlink", f"{HOME}/.config/systemd/user/default.target.wants/{n}", "") for n in names], signature="(sss)")

    @dbus.service.method(MANAGER, in_signature="asbb", out_signature="ba(sss)")
    def EnableUnitFiles(self, names, runtime, force):
        self.record(f"EnableUnitFiles {' '.join(names)} {bool(runtime)} {bool(force)}")
        self.set_file_state(names, "enabled")
        return True, dbus.Array([], signature="(sss)")

    @dbus.service.method(MANAGER)
    def Reload(self):
        self.record("Reload")

    @dbus.service.method(MANAGER)
    def Subscribe(self):
        self.record("Subscribe")

    @dbus.service.method(MANAGER)
    def Unsubscribe(self):
        self.record("Unsubscribe")

    @dbus.service.signal(MANAGER, signature="so")
    def UnitNew(self, name, path):
        pass

    @dbus.service.signal(MANAGER, signature="so")
    def UnitRemoved(self, name, path):
        pass

    @dbus.service.signal(MANAGER, signature="uoss")
    def JobRemoved(self, job_id, job_path, name, result):
        pass

    def finish_job(self, name):
        self.job += 1
        self.JobRemoved(dbus.UInt32(self.job), f"/org/freedesktop/systemd1/job/{self.job}", name, "done")

    @dbus.service.method(CONTROL, in_signature="ss")
    def Start(self, name, slice_):
        self.units[name] = Unit(self.bus, name, ("active", slice_, f"{HOME}/.config/systemd/user/{name}", "enabled", "disabled", 1))
        self.UnitNew(name, unit_path(name))
        self.finish_job(name)

    @dbus.service.method(CONTROL, in_signature="sss")
    def StartWithCgroup(self, name, slice_, cgroup):
        """Mirrors a real unit so the client reads that unit's real cgroup files."""
        self.units[name] = Unit(self.bus, name, ("active", slice_, "", "transient", "", 1), cgroup)
        if os.environ.get("REAL_RUNTIME"):
            self.mirrored[name] = os.environ["REAL_RUNTIME"]
        self.UnitNew(name, unit_path(name))
        self.finish_job(name)

    @dbus.service.method(CONTROL, in_signature="s")
    def Stop(self, name):
        unit = self.units.pop(name)
        unit.remove_from_connection()
        self.finish_job(name)
        self.UnitRemoved(name, unit_path(name))


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SESSION_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.systemd1", bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
