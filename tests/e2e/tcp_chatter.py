#!/usr/bin/env python3
"""Sends 256 KB over a localhost TCP pair every 100 ms for 300 s: steady TCP traffic in the caller's cgroup."""
import socket
import time

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind(("127.0.0.1", 0))
server.listen(1)
client = socket.create_connection(server.getsockname())
peer, _ = server.accept()
chunk = b"x" * 262144
end = time.monotonic() + 300
while time.monotonic() < end:
    client.sendall(chunk)
    received = 0
    while received < len(chunk):
        received += len(peer.recv(len(chunk) - received))
    time.sleep(0.1)
