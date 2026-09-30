#!/usr/bin/env python3
"""Mock greetd on the socket argv[1], appending every request to argv[2] as one JSON line.

Each create_session plays the next scenario from MOCK_SCENARIOS (comma separated), standing in for a PAM stack of
pam_gaze (sequential) then pam_unix:
  face     info "Look at the camera", then success once the info is acknowledged
  noface   info, a 2 s face check, info "Face not detected...", then a secret "Password:" prompt
  plain    a secret "Password:" prompt at once (nothing enrolled)
The password "right" succeeds; anything else is an auth_error. start_session and cancel_session always succeed.
"""
import json
import os
import socket
import struct
import sys
import time

sock_path, log_path = sys.argv[1], sys.argv[2]
scenarios = os.environ.get("MOCK_SCENARIOS", "face").split(",")


def send(conn, message):
    data = json.dumps(message).encode()
    conn.sendall(struct.pack("=I", len(data)) + data)


def recv(conn):
    head = conn.recv(4, socket.MSG_WAITALL)
    if len(head) < 4:
        return None
    (length,) = struct.unpack("=I", head)
    message = json.loads(conn.recv(length, socket.MSG_WAITALL))
    with open(log_path, "a") as out:
        out.write(json.dumps(message) + "\n")
    return message


def info(text):
    return {"type": "auth_message", "auth_message_type": "info", "auth_message": text}


SECRET = {"type": "auth_message", "auth_message_type": "secret", "auth_message": "Password:"}
SUCCESS = {"type": "success"}


def steps(name):
    """The replies of one PAM run: each is sent after the previous request, a callable judges the password."""
    if name == "face":
        return [info("Look at the camera"), SUCCESS]
    if name == "noface":
        return [info("Look at the camera"), ("sleep", 2.0), info("Face not detected. Enter your password."), SECRET,
                "password"]
    return [SECRET, "password"]


def run(conn):
    queue = []
    while True:
        request = recv(conn)
        if request is None:
            return
        kind = request["type"]
        if kind == "create_session":
            queue = steps(scenarios.pop(0) if scenarios else "plain")
        elif kind == "cancel_session":
            queue = []
            send(conn, SUCCESS)
            continue
        elif kind == "start_session":
            send(conn, SUCCESS)
            continue
        elif kind == "post_auth_message_response" and queue and queue[0] == "password":
            queue.pop(0)
            if request.get("response") == "right":
                send(conn, SUCCESS)
            else:
                send(conn, {"type": "error", "error_type": "auth_error", "description": "Authentication failed"})
            continue
        while queue and isinstance(queue[0], tuple):
            time.sleep(queue.pop(0)[1])
        send(conn, queue.pop(0) if queue else SUCCESS)


server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sock_path)
server.listen(1)
print("ready", flush=True)
while True:
    connection, _ = server.accept()
    run(connection)
    connection.close()
