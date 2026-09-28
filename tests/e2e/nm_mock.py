#!/usr/bin/env python3
"""A stand-in for NetworkManager on the test's system bus: the calls umbriel-linkd makes to join and leave a phone's
hotspot or Wi-Fi Direct group, recorded as JSON lines. Joining runs JOIN_CMD (the E2E brings up the link there);
leaving runs LEAVE_CMD. While the file FAIL_FLAG exists, a hotspot join ends deactivated, as a wrong passphrase would;
FAIL_FLAG-p2p does the same for a group. With P2P_PEER ("<name>=<hwaddr>"), a Wi-Fi P2P device is present and a find
turns up that peer.

Usage: nm_mock.py LOG JOIN_CMD LEAVE_CMD FAIL_FLAG [P2P_PEER]
"""
import json
import os
import subprocess
import sys
import time

from gi.repository import Gio, GLib

NM = "org.freedesktop.NetworkManager"
ROOT = "/org/freedesktop/NetworkManager"
ACTIVATED, DEACTIVATED = 2, 4
XML = f"""
<node>
  <interface name="{NM}">
    <method name="AddAndActivateConnection2">
      <arg direction="in" type="a{{sa{{sv}}}}" name="connection"/>
      <arg direction="in" type="o" name="device"/>
      <arg direction="in" type="o" name="specific_object"/>
      <arg direction="in" type="a{{sv}}" name="options"/>
      <arg direction="out" type="o" name="path"/>
      <arg direction="out" type="o" name="active_connection"/>
      <arg direction="out" type="a{{sv}}" name="result"/>
    </method>
    <method name="DeactivateConnection">
      <arg direction="in" type="o" name="active_connection"/>
    </method>
    <property name="Devices" type="ao" access="read"/>
  </interface>
  <interface name="{NM}.Connection.Active">
    <property name="State" type="u" access="read"/>
    <property name="Ip4Config" type="o" access="read"/>
  </interface>
  <interface name="{NM}.IP4Config">
    <property name="AddressData" type="aa{{sv}}" access="read"/>
  </interface>
  <interface name="{NM}.Device">
    <property name="DeviceType" type="u" access="read"/>
  </interface>
  <interface name="{NM}.Device.WifiP2P">
    <method name="StartFind">
      <arg direction="in" type="a{{sv}}" name="options"/>
    </method>
    <method name="StopFind"/>
    <property name="Peers" type="ao" access="read"/>
  </interface>
  <interface name="{NM}.WifiP2PPeer">
    <property name="Name" type="s" access="read"/>
    <property name="HwAddress" type="s" access="read"/>
  </interface>
</node>
"""


def main() -> int:
    log_path, join_cmd, leave_cmd, fail_flag = sys.argv[1:5]
    p2p_peer = sys.argv[5].split("=", 1) if len(sys.argv) > 5 else None
    info = Gio.DBusNodeInfo.new_for_xml(XML)
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
    state = {"count": 0, "active": {}, "finding": False}
    device, peer = f"{ROOT}/Devices/1", f"{ROOT}/P2PPeers/1"

    def log(entry):
        with open(log_path, "a") as out:
            out.write(json.dumps({**entry, "t": round(time.time(), 3)}) + "\n")

    def active_property(path):
        def get(_conn, _sender, _path, _iface, name):
            if name == "State":
                return GLib.Variant("u", state["active"].get(path, DEACTIVATED))
            return GLib.Variant("o", path.replace("ActiveConnection", "IP4Config"))
        return get

    def ip4_property(_conn, _sender, _path, _iface, _name):
        address = {"address": GLib.Variant("s", "10.80.0.1"), "prefix": GLib.Variant("u", 24)}
        return GLib.Variant("aa{sv}", [address])

    def call(_conn, _sender, _path, _iface, method, params, invocation):
        if method == "AddAndActivateConnection2":
            settings, _device, _specific, options = params.unpack()
            state["count"] += 1
            active = f"{ROOT}/ActiveConnection/{state['count']}"
            p2p = settings["connection"]["type"] == "wifi-p2p"
            failing = os.path.exists(fail_flag + "-p2p" if p2p else fail_flag)
            if p2p:
                log({
                    "call": "add-and-activate-p2p",
                    "peer": settings["wifi-p2p"]["peer"],
                    "device": _device,
                    "specific": _specific,
                    "autoconnect": settings["connection"]["autoconnect"],
                    "persist": options.get("persist"),
                    "fails": failing,
                })
            else:
                log({
                    "call": "add-and-activate",
                    "ssid": bytes(settings["802-11-wireless"]["ssid"]).decode(),
                    "psk": settings["802-11-wireless-security"]["psk"],
                    "key_mgmt": settings["802-11-wireless-security"]["key-mgmt"],
                    "autoconnect": settings["connection"]["autoconnect"],
                    "persist": options.get("persist"),
                    "fails": failing,
                })
            if not failing:
                subprocess.run(join_cmd, shell=True, check=True)
            state["active"][active] = DEACTIVATED if failing else ACTIVATED
            bus.register_object(active, info.interfaces[1], None, active_property(active), None)
            bus.register_object(active.replace("ActiveConnection", "IP4Config"), info.interfaces[2], None,
                                ip4_property, None)
            invocation.return_value(GLib.Variant("(ooa{sv})", (f"{ROOT}/Settings/{state['count']}", active, {})))
        elif method == "DeactivateConnection":
            (active,) = params.unpack()
            log({"call": "deactivate", "active": active})
            if state["active"].get(active) == ACTIVATED:
                subprocess.run(leave_cmd, shell=True, check=False)
            state["active"][active] = DEACTIVATED
            invocation.return_value(None)
        elif method in ("StartFind", "StopFind"):
            state["finding"] = method == "StartFind"
            log({"call": method})
            invocation.return_value(None)

    def root_property(_conn, _sender, _path, _iface, _name):
        return GLib.Variant("ao", [device] if p2p_peer else [])

    def device_property(_conn, _sender, _path, iface, name):
        if name == "DeviceType":
            return GLib.Variant("u", 30)
        return GLib.Variant("ao", [peer] if state["finding"] else [])

    def peer_property(_conn, _sender, _path, _iface, name):
        return GLib.Variant("s", p2p_peer[0] if name == "Name" else p2p_peer[1])

    bus.register_object(ROOT, info.interfaces[0], call, root_property, None)
    if p2p_peer:
        bus.register_object(device, info.interfaces[3], None, device_property, None)
        bus.register_object(device, info.interfaces[4], call, device_property, None)
        bus.register_object(peer, info.interfaces[5], None, peer_property, None)
    Gio.bus_own_name_on_connection(bus, NM, Gio.BusNameOwnerFlags.NONE, None, None)
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
