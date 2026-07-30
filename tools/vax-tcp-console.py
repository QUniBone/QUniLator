#!/usr/bin/env python3
"""Drive a simh console on a raw TCP port: wait for a prompt, type a line.

The same expect/send contract as tools/vax-console.mjs, for the vendored
simulator running on the workstation, whose console is a TCP socket rather
than the board's WebSocket.

    vax-tcp-console.py HOST:PORT --expect 'Username:' --send SYSTEM ...

--expect takes a regular expression matched against everything received since
the last step; --send types a line, character by character; --wait pauses;
--timeout sets the expect limit in milliseconds. Everything received is echoed
to stdout.
"""
import re
import socket
import sys
import time

args = sys.argv[1:]
host, port = args[0].rsplit(":", 1)
steps = []
i = 1
while i < len(args):
    if args[i] in ("--expect", "--send", "--send-hidden", "--wait", "--timeout"):
        steps.append((args[i][2:], args[i + 1]))
        i += 2
    else:
        sys.exit(f"unknown argument {args[i]}")

sock = socket.create_connection((host, int(port)), timeout=5)
sock.settimeout(0.1)
buf = b""


def pump():
    global buf
    try:
        data = sock.recv(4096)
        if data:
            buf += data
            sys.stdout.write(data.decode("latin1"))
            sys.stdout.flush()
    except socket.timeout:
        pass


def expect(pattern, timeout_ms, mark):
    deadline = time.time() + timeout_ms / 1000.0
    rx = re.compile(pattern.encode("latin1"))
    while time.time() < deadline:
        m = rx.search(buf, mark)
        if m:
            return m.end()
        pump()
    raise SystemExit(f"\n--- timed out waiting for /{pattern}/")


def send_line(text, hidden):
    for ch in text.encode("latin1"):
        sock.send(bytes([ch]))
        time.sleep(0.03)
        pump()
    sock.send(b"\r")


time.sleep(1.0)
pump()
mark = len(buf)
timeout_ms = 30000
for kind, value in steps:
    if kind == "timeout":
        timeout_ms = int(value)
    elif kind == "wait":
        deadline = time.time() + int(value) / 1000.0
        while time.time() < deadline:
            pump()
    elif kind == "expect":
        mark = expect(value, timeout_ms, mark)
    elif kind in ("send", "send-hidden"):
        send_line(value, kind == "send-hidden")
        mark = len(buf)
time.sleep(1.0)
pump()
print()
