#!/usr/bin/env python3
"""Compares org.andromeda.Link1 as a running daemon introspects it with api/org.andromeda.Link1.xml, the API's one
source: the same methods, signals, and properties, with the same argument names, types, and directions. Usage:
link_api_check.py API_XML LIVE_XML; exits 1 and prints each difference."""
import sys
import xml.etree.ElementTree as ET

INTERFACE = "org.andromeda.Link1"


def members(path):
    root = ET.parse(path).getroot()
    interface = next(node for node in root.iter("interface") if node.get("name") == INTERFACE)
    found = {}
    for member in interface:
        if member.tag in ("method", "signal"):
            args = []
            for arg in member.iter("arg"):
                direction = arg.get("direction", "in" if member.tag == "method" else "out")
                # zbus introspects a method's out arguments without names.
                name = None if member.tag == "method" and direction == "out" else arg.get("name")
                args.append((name, arg.get("type"), direction))
            found[(member.tag, member.get("name"))] = args
        elif member.tag == "property":
            found[("property", member.get("name"))] = [(member.get("type"), member.get("access"))]
    return found


def main():
    source, live = members(sys.argv[1]), members(sys.argv[2])
    differences = []
    for key in sorted(source.keys() | live.keys()):
        if key not in live:
            differences.append(f"{key[0]} {key[1]}: in the API, not in the daemon")
        elif key not in source:
            differences.append(f"{key[0]} {key[1]}: in the daemon, not in the API")
        elif source[key] != live[key]:
            differences.append(f"{key[0]} {key[1]}: API {source[key]}, daemon {live[key]}")
    for difference in differences:
        print(difference)
    sys.exit(1 if differences else 0)


main()
