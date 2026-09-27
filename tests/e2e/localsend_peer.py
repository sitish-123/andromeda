#!/usr/bin/env python3
"""An independent LocalSend v2 peer for tests/e2e/link_localsend.sh, written from the protocol spec
(https://github.com/localsend/protocol) with the standard library only: it announces itself by multicast, serves
HTTPS register, prepare-upload, and upload, saves what it receives, and prints every event as a JSON line.

Usage: localsend_peer.py --cert C --key K --out DIR [--alias A] [--fingerprint F]
"""
import argparse
import hashlib
import http.server
import json
import os
import socket
import ssl
import struct
import sys
import threading
import urllib.parse

MULTICAST = "224.0.0.167"
PORT = 53317


def say(**event):
    print(json.dumps(event), flush=True)


def register(host, port, info):
    """Answers an announcement the way the spec asks: a register POST to the announcer."""
    import http.client
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    connection = http.client.HTTPSConnection(host, port, context=context, timeout=5)
    try:
        connection.request("POST", "/api/localsend/v2/register", json.dumps(info), {"Content-Type": "application/json"})
        say(event="registered", status=connection.getresponse().status)
    except OSError as error:
        say(event="register-failed", error=str(error))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--alias", default="E2E LocalSend")
    parser.add_argument("--fingerprint", help="announce this instead of the certificate's own")
    args = parser.parse_args()
    der = ssl.PEM_cert_to_DER_cert(open(args.cert).read())
    fingerprint = args.fingerprint or hashlib.sha256(der).hexdigest()
    info = {"alias": args.alias, "version": "2.1", "deviceModel": "E2E", "deviceType": "headless",
            "fingerprint": fingerprint, "port": PORT, "protocol": "https", "download": False}
    sessions = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, status, body=None):
            data = json.dumps(body).encode() if body is not None else b""
            self.send_response(status)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            url = urllib.parse.urlparse(self.path)
            query = dict(urllib.parse.parse_qsl(url.query))
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if url.path == "/api/localsend/v2/register":
                say(event="register", peer=json.loads(body))
                self.reply(200, info)
            elif url.path == "/api/localsend/v2/prepare-upload":
                request = json.loads(body)
                files = {key: f"token-{key}" for key in request["files"]}
                sessions["s1"] = {key: request["files"][key] for key in files}
                say(event="prepare-upload", files=request["files"], sender=request["info"])
                self.reply(200, {"sessionId": "s1", "files": files})
            elif url.path == "/api/localsend/v2/upload":
                meta = sessions.get(query.get("sessionId"), {}).get(query.get("fileId"))
                if meta is None or query.get("token") != f"token-{query.get('fileId')}":
                    self.reply(403)
                    return
                path = os.path.join(args.out, os.path.basename(meta["fileName"]))
                open(path, "wb").write(body)
                say(event="upload", name=meta["fileName"], bytes=len(body),
                    sha256=hashlib.sha256(body).hexdigest(), announced_sha256=meta.get("sha256"))
                self.reply(200)
            else:
                self.reply(404)

    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("", PORT))
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4sl", socket.inet_aton(MULTICAST), socket.INADDR_ANY))
    udp.sendto(json.dumps({**info, "announce": True}).encode(), (MULTICAST, PORT))
    say(event="ready", fingerprint=fingerprint)
    while True:
        data, source = udp.recvfrom(65536)
        try:
            announcement = json.loads(data)
        except ValueError:
            continue
        if announcement.get("fingerprint") == fingerprint:
            continue
        say(event="announcement", source=source[0], announcement=announcement)
        if announcement.get("announce"):
            register(source[0], announcement.get("port", PORT), info)


if __name__ == "__main__":
    sys.exit(main())
