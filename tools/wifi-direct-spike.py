#!/usr/bin/env python3
"""Wi-Fi Direct between this laptop (NetworkManager over iwd) and a phone, before Link depends on it. Proves or
disproves, and writes each step to artifacts/wifi-direct-spike/results.jsonl:

1. discovery finds the phone while its Settings > Wi-Fi > Wi-Fi Direct screen is open;
2. a push-button connect is accepted on the phone and the laptop gets an address on the group;
3. the laptop's own Wi-Fi connection stays up throughout (the reason to prefer this over the hotspot);
4. throughput over the group, and over the shared Wi-Fi for comparison, into `nc` on the phone over adb.

Usage: tools/wifi-direct-spike.py PHONE_NAME [MIB]   (the name the phone's Wi-Fi Direct screen shows; USB adb on)
"""
import json
import pathlib
import socket
import subprocess
import sys
import time

from gi.repository import Gio, GLib

NM = "org.freedesktop.NetworkManager"
NM_PATH = "/org/freedesktop/NetworkManager"
P2P_IFACE = f"{NM}.Device.WifiP2P"
DEVICE_TYPE_WIFI, DEVICE_TYPE_P2P = 2, 30
ACTIVATED = 2
PORT = 5001
OUT = pathlib.Path(__file__).resolve().parent.parent / "artifacts" / "wifi-direct-spike"
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)


def record(step, **fields):
    line = json.dumps({"step": step, "t": time.strftime("%H:%M:%S"), **fields})
    with open(OUT / "results.jsonl", "a") as out:
        out.write(line + "\n")
    print(line, flush=True)


def fail(step, why):
    record(step, ok=False, why=why)
    sys.exit(1)


def get(path, iface, prop):
    reply = bus.call_sync(NM, path, "org.freedesktop.DBus.Properties", "Get", GLib.Variant("(ss)", (iface, prop)),
                          GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, -1, None)
    return reply.unpack()[0]


def call(path, iface, method, args=None, reply=None):
    return bus.call_sync(NM, path, iface, method, args, GLib.VariantType(reply) if reply else None,
                         Gio.DBusCallFlags.NONE, 120000, None).unpack()


def device(kind):
    for path in get(NM_PATH, NM, "Devices"):
        if get(path, f"{NM}.Device", "DeviceType") == kind:
            return path
    return None


def station(wifi):
    active = get(wifi, f"{NM}.Device", "ActiveConnection")
    return None if active == "/" else get(active, f"{NM}.Connection.Active", "Id")


def wait(what, seconds, check):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if (value := check()) is not None:
            return value
        ctx = GLib.MainContext.default()
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.5)
    return None


def adb(*args, background=False):
    command = ["adb", "shell", *args]
    if background:
        return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return subprocess.run(command, capture_output=True, text=True, timeout=30).stdout.strip()


def throughput(host, mib):
    """MiB/s of `mib` MiB written into `nc -l` on the phone at `host`."""
    listener = adb(f"toybox nc -l -p {PORT} > /dev/null", background=True)
    time.sleep(1.5)
    chunk = bytes(1 << 20)
    try:
        with socket.create_connection((host, PORT), timeout=10) as sock:
            started = time.monotonic()
            for _ in range(mib):
                sock.sendall(chunk)
            sock.shutdown(socket.SHUT_WR)
            sock.recv(1)
            seconds = time.monotonic() - started
    finally:
        listener.wait(timeout=30)
    return round(mib / seconds, 2), round(seconds, 2)


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    phone, mib = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 200
    OUT.mkdir(parents=True, exist_ok=True)
    p2p, wifi = device(DEVICE_TYPE_P2P), device(DEVICE_TYPE_WIFI)
    if p2p is None:
        fail("p2p-device", "NetworkManager has no Wi-Fi P2P device")
    home = station(wifi)
    record("start", phone=phone, home_wifi=home, backend="iwd" if "/net/connman" in get(p2p, f"{NM}.Device", "Udi") else "wpa_supplicant")
    if adb("echo", "ok") != "ok":
        fail("adb", "no phone on adb")

    lan_ip = adb("ip -4 -o addr show wlan0 | awk '{print $4}' | cut -d/ -f1")
    if home and lan_ip:
        rate, seconds = throughput(lan_ip, mib)
        record("shared-wifi-throughput", host=lan_ip, mib=mib, seconds=seconds, mib_per_s=rate)

    print(f"Open Settings > Wi-Fi > Wi-Fi Direct on the phone, named {phone!r}.", flush=True)
    call(p2p, P2P_IFACE, "StartFind", GLib.Variant("(a{sv})", ({"timeout": GLib.Variant("i", 120)},)))
    started = time.monotonic()

    def found():
        for peer in get(p2p, P2P_IFACE, "Peers"):
            if get(peer, f"{NM}.WifiP2PPeer", "Name") == phone:
                return peer
        return None

    peer = wait("peer", 120, found)
    if peer is None:
        fail("discovery", f"{phone!r} not found in 120 s")
    hwaddr = get(peer, f"{NM}.WifiP2PPeer", "HwAddress")
    record("discovery", ok=True, seconds=round(time.monotonic() - started, 1), peer=hwaddr)
    call(p2p, P2P_IFACE, "StopFind")

    settings = {
        "connection": {"id": GLib.Variant("s", "umbriel-p2p-spike"), "type": GLib.Variant("s", "wifi-p2p"),
                       "autoconnect": GLib.Variant("b", False)},
        "wifi-p2p": {"peer": GLib.Variant("s", hwaddr)},
        "ipv4": {"method": GLib.Variant("s", "auto")},
        "ipv6": {"method": GLib.Variant("s", "ignore")},
    }
    print("Accept the invitation on the phone.", flush=True)
    started = time.monotonic()
    _, active, _ = call(NM_PATH, NM, "AddAndActivateConnection2",
                        GLib.Variant("(a{sa{sv}}ooa{sv})", (settings, p2p, peer, {"persist": GLib.Variant("s", "volatile")})),
                        "(ooa{sv})")
    try:
        state = wait("activated", 90, lambda: ACTIVATED if get(active, f"{NM}.Connection.Active", "State") == ACTIVATED else None)
        if state is None:
            fail("connect", "the group did not come up in 90 s (invitation declined or not interoperable)")
        config = get(active, f"{NM}.Connection.Active", "Ip4Config")
        addresses = get(config, f"{NM}.IP4Config", "AddressData") if config != "/" else []
        gateway = get(config, f"{NM}.IP4Config", "Gateway") if config != "/" else ""
        record("connect", ok=True, seconds=round(time.monotonic() - started, 1),
               address=addresses[0]["address"] if addresses else None, group_owner=gateway)
        still = station(wifi)
        record("home-wifi-kept", ok=still == home, before=home, during=still)
        # With the laptop as group owner there is no gateway; the phone's end is its p2p interface.
        target = gateway or adb("ip -4 -o addr | awk '/p2p/ {print $4}' | cut -d/ -f1 | head -1")
        if target:
            rate, seconds = throughput(target, mib)
            record("wifi-direct-throughput", host=target, mib=mib, seconds=seconds, mib_per_s=rate)
    finally:
        try:
            call(NM_PATH, NM, "DeactivateConnection", GLib.Variant("(o)", (active,)))
        except GLib.Error as error:
            print(f"deactivating: {error.message}", file=sys.stderr)
    record("done", home_wifi_after=station(wifi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
