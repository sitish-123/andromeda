#!/usr/bin/env python3
"""A UDP relay that impairs one direction, for testing how a QUIC peer copes with a bad link without root.

Usage: udp_impair.py LISTEN_PORT TARGET_PORT CONTROL STATS

Datagrams from any client to LISTEN_PORT go to 127.0.0.1:TARGET_PORT from a socket per client, and answers go back.
The client-to-target direction is impaired as CONTROL says, re-read each second: a line such as
"rate=2000000 loss=0.03 delay=0.02" (bits per second, a drop probability, seconds), or "clean". A rate limit queues
at most 100 ms of data and drops the rest. Each second STATS gets one line: time, forwarded, and dropped datagrams.
"""
import heapq
import random
import selectors
import socket
import sys
import time

QUEUE_S = 0.1


def read_control(path):
    settings = {"rate": 0.0, "loss": 0.0, "delay": 0.0}
    try:
        text = open(path).read().split()
    except OSError:
        return settings
    for item in text:
        key, _, value = item.partition("=")
        if key in settings:
            settings[key] = float(value)
    return settings


def main():
    listen_port, target_port, control, stats_path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    target = ("127.0.0.1", target_port)
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.bind(("0.0.0.0", listen_port))
    front.setblocking(False)
    selector = selectors.DefaultSelector()
    selector.register(front, selectors.EVENT_READ, None)
    upstreams = {}
    pending = []
    settings = read_control(control)
    next_free = 0.0
    forwarded = dropped = 0
    tick = time.monotonic() + 1
    stats = open(stats_path, "a", buffering=1)
    while True:
        now = time.monotonic()
        timeout = min([tick - now] + [pending[0][0] - now for _ in pending[:1]])
        for key, _ in selector.select(max(timeout, 0)):
            if key.data is None:
                data, client = front.recvfrom(65535)
                upstream = upstreams.get(client)
                if upstream is None:
                    upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    upstream.bind(("127.0.0.1", 0))
                    upstream.setblocking(False)
                    upstreams[client] = upstream
                    selector.register(upstream, selectors.EVENT_READ, client)
                now = time.monotonic()
                if random.random() < settings["loss"]:
                    dropped += 1
                    continue
                at = now
                if settings["rate"] > 0:
                    next_free = max(now, next_free) + len(data) * 8 / settings["rate"]
                    if next_free - now > QUEUE_S:
                        next_free -= len(data) * 8 / settings["rate"]
                        dropped += 1
                        continue
                    at = next_free
                heapq.heappush(pending, (at + settings["delay"], id(data), upstream, data))
            else:
                data, _ = key.fileobj.recvfrom(65535)
                front.sendto(data, key.data)
        now = time.monotonic()
        while pending and pending[0][0] <= now:
            _, _, upstream, data = heapq.heappop(pending)
            upstream.sendto(data, target)
            forwarded += 1
        if now >= tick:
            print(f"{time.time():.1f} {forwarded} {dropped}", file=stats)
            forwarded = dropped = 0
            settings = read_control(control)
            tick = now + 1


if __name__ == "__main__":
    main()
