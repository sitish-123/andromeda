#!/usr/bin/env python3
"""A desktop MPRIS player for the E2E tests: org.mpris.MediaPlayer2.e2e on the session bus.

Plays a fixed track, obeys Play, Pause, PlayPause, Next, SetPosition, Seek, and Volume like a real player (emitting
PropertiesChanged and Seeked), and appends every call to the file named by argv[1]. argv[2], when given, replaces the
"e2e" in the bus name and drops mpris:trackid, so a seek must use the relative Seek. org.e2e.Test.Advance moves the
position without a signal, as playback does.
"""
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ROOT = "org.mpris.MediaPlayer2"
PLAYER = "org.mpris.MediaPlayer2.Player"
PROPS = "org.freedesktop.DBus.Properties"
TRACK = dbus.ObjectPath("/org/e2e/track/1")


class Player(dbus.service.Object):
    def __init__(self, bus, log, track):
        super().__init__(bus, "/org/mpris/MediaPlayer2")
        self.log = log
        self.track = track
        self.status = "Playing"
        self.title = "Desk Session"
        self.position = 12_000_000
        self.volume = 0.5

    def record(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    def player_props(self):
        return {
            "PlaybackStatus": self.status,
            "Metadata": dbus.Dictionary({
                "xesam:title": self.title,
                "xesam:artist": dbus.Array(["The Testers"], signature="s"),
                "xesam:album": "E2E",
                "mpris:length": dbus.Int64(200_000_000),
            } | ({"mpris:trackid": TRACK} if self.track else {}), signature="sv"),
            "Position": dbus.Int64(self.position),
            "Volume": dbus.Double(self.volume),
            "Rate": dbus.Double(1.0),
            "CanPlay": True, "CanPause": True, "CanGoNext": True, "CanGoPrevious": True,
            "CanSeek": True, "CanControl": True,
        }

    def changed(self, *names):
        props = self.player_props()
        self.PropertiesChanged(PLAYER, {name: props[name] for name in names}, [])

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.GetAll(interface)[name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface == ROOT:
            return {"Identity": "E2E Player", "CanQuit": False, "CanRaise": False, "HasTrackList": False}
        return self.player_props()

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, interface, name, value):
        self.record(f"Set {name} {float(value):.2f}")
        if name == "Volume":
            self.volume = float(value)
            self.changed("Volume")

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.signal(PLAYER, signature="x")
    def Seeked(self, position):
        pass

    @dbus.service.method(PLAYER)
    def Play(self):
        self.record("Play")
        self.status = "Playing"
        self.changed("PlaybackStatus")

    @dbus.service.method(PLAYER)
    def Pause(self):
        self.record("Pause")
        self.status = "Paused"
        self.changed("PlaybackStatus")

    @dbus.service.method(PLAYER)
    def PlayPause(self):
        self.record("PlayPause")
        self.status = "Paused" if self.status == "Playing" else "Playing"
        self.changed("PlaybackStatus")

    @dbus.service.method(PLAYER)
    def Next(self):
        self.record("Next")
        self.title = "Desk Session, part 2"
        self.position = 0
        self.changed("Metadata", "Position")

    @dbus.service.method(PLAYER)
    def Previous(self):
        self.record("Previous")

    @dbus.service.method(PLAYER, in_signature="ox")
    def SetPosition(self, track, position):
        self.record(f"SetPosition {track} {position}")
        self.position = int(position)
        self.Seeked(dbus.Int64(self.position))

    @dbus.service.method(PLAYER, in_signature="x")
    def Seek(self, offset):
        self.record(f"Seek {offset}")
        self.position = max(0, self.position + int(offset))
        self.Seeked(dbus.Int64(self.position))

    @dbus.service.method("org.e2e.Test", in_signature="x")
    def Advance(self, microseconds):
        self.position += int(microseconds)


DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
suffix = sys.argv[2] if len(sys.argv) > 2 else None
player = Player(bus, sys.argv[1], track=suffix is None)
name = dbus.service.BusName(f"org.mpris.MediaPlayer2.{suffix or 'e2e'}", bus)
GLib.MainLoop().run()
