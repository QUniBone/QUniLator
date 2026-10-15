#!/usr/bin/env python3
"""Send an image upload that stops halfway and check the board refuses it.

    cut-upload.py <host> <user> <password> <dir/name.dsk>

The request announces a 4 MB file and sends half of it before shutting its
sending side, which is what a client that goes away mid-upload looks like to
the board. Exits 0 when the board answers with an error, 1 (printing what it
answered) when it claims the upload went through.
"""

import base64
import os
import socket
import sys


def main():
    host, user, password, target = sys.argv[1:5]
    directory, name = target.rsplit("/", 1)
    auth = base64.b64encode(f"{user}:{password}".encode()).decode()
    boundary = "----qunilator-ci-cut"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{name}"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + os.urandom(4 * 1024 * 1024) + f"\r\n--{boundary}--\r\n".encode()
    head = (
        f"POST /api/images?dir={directory} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Authorization: Basic {auth}\r\n"
        f"Content-Type: multipart/form-data; boundary={boundary}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode()

    with socket.create_connection((host, 80), timeout=60) as s:
        s.sendall(head + body[: len(body) // 2])
        s.shutdown(socket.SHUT_WR)
        reply = b""
        while chunk := s.recv(4096):
            reply += chunk

    text = reply.decode(errors="replace")
    status = text.split(" ", 2)[1] if text.startswith("HTTP/") else "none"
    if status != "200" and '"ok":true' not in text:
        return 0
    answer = text.split("\r\n\r\n", 1)[-1][:200]
    print(f"An upload cut off halfway answered HTTP {status}: {answer}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
