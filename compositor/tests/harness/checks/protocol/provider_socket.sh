#!/usr/bin/env bash
# The provider socket: each client gets hello (manifest hash, action entries) and a snapshot, a call returns its
# result after the events it caused (each with the call id as cause), every client sees those events, and bad calls
# return the contract error object.
set -euo pipefail

printf '\n[output.HEADLESS-1]\nmin_workspaces = 2\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

python3 - "$ANDROMEDA_RUNTIME_DIR/andromeda-provider-wayland-0.sock" << 'PY'
import json
import socket
import subprocess
import sys


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.lines = self.sock.makefile("r", encoding="utf-8")

    def read(self):
        line = self.lines.readline()
        assert line, "provider closed the connection"
        return json.loads(line)

    def call(self, call_id, action, args):
        self.sock.sendall((json.dumps({"op": "call", "id": call_id, "action": action, "args": args,
                                       "actor": "check"}) + "\n").encode())
        events = []
        while True:
            message = self.read()
            if message["op"] == "result":
                assert message["id"] == call_id, message
                assert message["schema_version"] == 1, message
                return events, message
            events.append(message)


def check(condition, what):
    if not condition:
        print(f"FAIL: {what}")
        sys.exit(1)


path = sys.argv[1]
a = Client(path)
hello = a.read()
check(hello["op"] == "hello" and hello["provider"] == "compositor" and hello["schema_version"] == 1, hello)
check(len(hello["manifest_hash"]) == 64, f"manifest hash: {hello['manifest_hash']}")
entries = {entry["name"]: entry for entry in hello["actions"]}
check({"window.list", "window.focus", "window.close", "window.move_to_workspace", "workspace.list",
       "workspace.switch", "output.list"} <= entries.keys(), f"hello actions: {sorted(entries)}")
check(entries["workspace.switch"]["permission"] == "windows", entries["workspace.switch"])
snapshot = a.read()
check(snapshot["op"] == "snapshot" and snapshot["seq"] == 0, snapshot)
state = snapshot["state"]
check(set(state) == {"windows", "workspaces", "outputs"}, f"snapshot keys: {sorted(state)}")
check([o["id"] for o in state["outputs"]] == ["out:HEADLESS-1"], state["outputs"])
focused = [w for w in state["workspaces"] if w["focused"]]
check(len(focused) == 1 and focused[0]["index"] == 1 and focused[0]["id"].startswith("ws"), state["workspaces"])
check(any(w["index"] == 2 for w in state["workspaces"]), f"no second workspace: {state['workspaces']}")

b = Client(path)
check(b.read()["op"] == "hello" and b.read()["seq"] == 0, "second client hello and snapshot")

events, result = a.call("a1", "workspace.switch", {"index": 2})
check(result["ok"] and result["value"]["index"] == 2 and result["value"]["focused"], result)
check(events and all(e["op"] == "event" and e["cause"] == "a1" for e in events), f"events before result: {events}")
check([e["seq"] for e in events] == list(range(1, len(events) + 1)), f"event seqs: {events}")
patched = {**state}
for event in events:
    patched.update(event["delta"])
switched = [w for w in patched["workspaces"] if w["focused"]]
check(len(switched) == 1 and switched[0]["index"] == 2, f"merge-patched workspaces: {patched['workspaces']}")
check(result["changed"]["workspaces"] == patched["workspaces"], f"changed: {result['changed']}")
output = patched["outputs"][0]
check(output["active_workspace"] == switched[0]["id"], f"output after switch: {output}")
seen_by_b = [b.read() for _ in events]
check([e["cause"] for e in seen_by_b] == ["a1"] * len(events), f"second client events: {seen_by_b}")

events, result = a.call("a2", "workspace.switch", {"index": 2})
check(result["ok"] and events == [] and result["changed"] == {}, f"repeated switch changed state: {events} {result}")

foot = subprocess.Popen(["foot", "--title=provider", "sh", "-c", "sleep 120"], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
try:
    windows = []
    while not windows:
        event = a.read()
        check(event["op"] == "event" and event["cause"] is None, f"unprompted event: {event}")
        windows = event["delta"].get("windows", windows)
    window = windows[0]
    check(window["id"].startswith("w") and window["app_id"] == "foot", window)

    _, result = a.call("a3", "window.focus", {"id": "w99"})
    check(not result["ok"] and result["error"]["code"] == "not_found", result)
    check(window["id"] in result["error"]["hint"], result)
    check(result["error"]["example"] == "andromeda act window.focus --id w1", result)

    _, result = a.call("a3b", "window.focus", {"id": window["id"]})
    check(result["ok"] and result["value"]["id"] == window["id"] and result["value"]["focused"], result)
finally:
    foot.kill()

_, result = a.call("a4", "workspace.swich", {"index": 1})
check(result["error"]["code"] == "not_found" and "workspace.switch" in result["error"]["hint"], result)

_, result = a.call("a5", "workspace.switch", {"index": "two"})
error = result["error"]
check(error["code"] == "invalid_argument" and error["example"] == "andromeda act workspace.switch --index 2", error)
check(set(error) == {"code", "message", "hint", "example"}, error)
PY
