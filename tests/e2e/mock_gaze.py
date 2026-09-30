#!/usr/bin/env python3
"""Mock com.gundulabs.Gaze (and login1's PrepareForSleep) on DBUS_SYSTEM_BUS_ADDRESS.

Every Gaze call is appended to argv[1]. The test drives it through dsk.test.Gaze: SetEnrolled(b), SetClaimFails(b),
Finish(result, rgb, ir, judged) ends the running verify with VerifyStatus, Status(s) sends FaceStatus, Sleep(b) sends
PrepareForSleep, and Quit() exits as if gazed died. Answers PamInternal like gazed.
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

GAZE = "com.gundulabs.Gaze"
TEST = "dsk.test.Gaze"
LOGIN = "org.freedesktop.login1.Manager"
PROPS = "org.freedesktop.DBus.Properties"


def record(line):
    with open(sys.argv[1], "a") as out:
        out.write(line + "\n")


class Gaze(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/com/gundulabs/Gaze")
        self.enrolled = True
        self.claim_fails = False
        self.claimant = None
        self.pam_internal = []

    @dbus.service.method(GAZE, in_signature="s", out_signature="b")
    def HasEnrolledFaces(self, username):
        record(f"HasEnrolledFaces {username}")
        return self.enrolled

    @dbus.service.method(GAZE, in_signature="s", sender_keyword="sender")
    def Claim(self, username, sender):
        record(f"Claim {username}")
        if self.claim_fails or self.claimant not in (None, sender):
            raise dbus.exceptions.DBusException("the camera is in use", name="org.freedesktop.DBus.Error.Failed")
        self.claimant = sender

    @dbus.service.method(GAZE, sender_keyword="sender")
    def Release(self, sender):
        record("Release")
        if self.claimant == sender:
            self.claimant = None

    @dbus.service.method(GAZE, in_signature="ss")
    def VerifyStartFor(self, face, service):
        record(f"VerifyStartFor {service}")

    @dbus.service.method(GAZE)
    def VerifyStop(self):
        record("VerifyStop")

    @dbus.service.method(GAZE, in_signature="s")
    def AddPamInternal(self, service):
        record(f"AddPamInternal {service}")
        if service not in self.pam_internal:
            self.pam_internal.append(service)

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        if prop == "PamInternal":
            return dbus.Array(self.pam_internal, signature="s")
        raise dbus.exceptions.DBusException(prop, name="org.freedesktop.DBus.Error.UnknownProperty")

    @dbus.service.signal(GAZE, signature="s")
    def FaceStatus(self, status):
        pass

    @dbus.service.signal(GAZE, signature="sa(sddbddb)ss")
    def VerifyStatus(self, result, faces, rgb, ir):
        pass

    @dbus.service.method(TEST, in_signature="b")
    def SetEnrolled(self, enrolled):
        self.enrolled = bool(enrolled)

    @dbus.service.method(TEST, in_signature="b")
    def SetClaimFails(self, fails):
        self.claim_fails = bool(fails)

    @dbus.service.method(TEST, in_signature="s")
    def Status(self, status):
        self.FaceStatus(status)

    @dbus.service.method(TEST, in_signature="sssb")
    def Finish(self, result, rgb, ir, judged):
        faces = [("default", 0.62, 0.0, True, 0.9, 0.0, False)] if judged else []
        self.VerifyStatus(result, dbus.Array(faces, signature="(sddbddb)"), rgb, ir)

    @dbus.service.method(TEST)
    def Quit(self):
        GLib.idle_add(loop.quit)


class Login(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/login1")

    @dbus.service.signal(LOGIN, signature="b")
    def PrepareForSleep(self, sleeping):
        pass

    @dbus.service.method(TEST, in_signature="b")
    def Sleep(self, sleeping):
        self.PrepareForSleep(sleeping)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
names = [dbus.service.BusName(GAZE, bus), dbus.service.BusName("org.freedesktop.login1", bus)]
gaze = Gaze(bus)
login = Login(bus)
loop = GLib.MainLoop()
print("ready", flush=True)
loop.run()
