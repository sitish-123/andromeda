#!/usr/bin/env python3
"""A Docker Engine API on a unix socket for Activity E2E tests.

Usage: mock_docker.py SOCKET CALLS PID [--chunked]
Serves GET /version, GET /containers/json, GET /containers/<id>/json and POST /containers/<id>/{stop,pause,unpause}.
Each GET appends "GET <path>" to CALLS, each POST "<verb> <name>". The running containers' State.Pid is PID, so the
client reads that process's real cgroup. --chunked sends the container list as an HTTP/1.1 chunked body, as Docker
does for large lists. Prints "ready" once it listens.
"""
import http.server
import json
import os
import socketserver
import sys
import time

SOCKET, CALLS, PID = sys.argv[1], sys.argv[2], int(sys.argv[3])
CHUNKED = "--chunked" in sys.argv[4:]
NOW = int(time.time())
CONTAINERS = {
    "a1" * 32: {"Names": ["/web"], "Image": "nginx:1.27-alpine", "State": "running", "Status": "Up 2 hours"},
    "b2" * 32: {"Names": ["/cache"], "Image": "redis:7", "State": "running", "Status": "Up 2 hours"},
    "c3" * 32: {"Names": ["/build"], "Image": "node:22", "State": "exited", "Status": "Exited (0) 1 hour ago"},
}


class Handler(http.server.BaseHTTPRequestHandler):
    def address_string(self):
        return "unix"

    def log_message(self, *args):
        pass

    def reply(self, status, body=None):
        data = json.dumps(body).encode() if body is not None else b""
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def record(self, line):
        with open(CALLS, "a") as f:
            f.write(line + "\n")

    def chunked(self, body):
        data = json.dumps(body).encode()
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
        for start in range(0, len(data), 7):
            piece = data[start:start + 7]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
        self.wfile.write(b"0\r\n\r\n")

    def do_GET(self):
        path = self.path.split("?")[0]
        self.record(f"GET {path}")
        if path == "/version":
            return self.reply(200, {"Version": "27.0.0", "Components": [{"Name": "Engine", "Version": "27.0.0"}]})
        if path == "/containers/json":
            if CHUNKED:
                return self.chunked([{"Id": cid, "Created": NOW - 7200, **c} for cid, c in CONTAINERS.items()])
            return self.reply(200, [{"Id": cid, "Created": NOW - 7200, **c} for cid, c in CONTAINERS.items()])
        parts = path.split("/")
        if len(parts) == 4 and parts[1] == "containers" and parts[3] == "json" and parts[2] in CONTAINERS:
            c = CONTAINERS[parts[2]]
            return self.reply(200, {"Id": parts[2], "State": {"Pid": PID if c["State"] in ("running", "paused") else 0}})
        self.reply(404, {"message": f"no such path {path}"})

    def do_POST(self):
        parts = self.path.split("?")[0].split("/")
        if len(parts) != 4 or parts[1] != "containers" or parts[2] not in CONTAINERS:
            return self.reply(404, {"message": "No such container"})
        c, verb = CONTAINERS[parts[2]], parts[3]
        self.record(f"{verb} {c['Names'][0][1:]}")
        if verb == "stop":
            if c["State"] == "exited":
                return self.reply(304)
            c["State"], c["Status"] = "exited", "Exited (0) 1 second ago"
        elif verb == "pause" and c["State"] == "running":
            c["State"], c["Status"] = "paused", "Up 2 hours (Paused)"
        elif verb == "unpause" and c["State"] == "paused":
            c["State"], c["Status"] = "running", "Up 2 hours"
        else:
            return self.reply(409, {"message": f"container is {c['State']}"})
        self.reply(204)


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


if os.path.exists(SOCKET):
    os.unlink(SOCKET)
with Server(SOCKET, Handler) as server:
    print("ready", flush=True)
    server.serve_forever()
