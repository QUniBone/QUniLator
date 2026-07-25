#!/usr/bin/env python3
"""Reliable console driver for the QBone external console (/ws/console/ext).

The 11/73's on-board SLU has a one-character receive buffer and no RX FIFO, so
sending input faster than the guest drains it overruns the register and drops
characters — which is why blind paced-sending (even at 0.1 s/char) corrupts long
lines during RSX NETGEN and similar interactive work.

This driver sends input **echo-driven**: it transmits one character, waits until
the guest echoes it back, then sends the next. Because a full-duplex guest (RSX,
2.11BSD, ODT) only echoes a character after its console driver has read it out of
the SLU, the sender can never outrun the receive buffer. That makes scripted
input reliable regardless of how busy the guest is.

Usage:
    console_send.py "expect prompt" "send line" ["expect ..." "send ..." ...]
    console_send.py -f script.txt          # a file of expect:/send: lines
    console_send.py --interactive          # expect the prompt, then read stdin

An expect step waits for a substring/regex in the guest output; a send step
types a line (echo-driven) and appends CR. See --help.

Auth: HTTP basic, any user, password from ~/.qbone-pw (or --pw-file). Host from
--host (default qbone).
"""
import argparse
import base64
import os
import re
import sys
import time

try:
    from websocket import create_connection  # websocket-client
except ImportError:
    sys.exit("need websocket-client: pip install websocket-client")


class Console:
    def __init__(self, host, pw, echo_timeout=2.0, quiet=False):
        auth = base64.b64encode((":" + pw).encode()).decode()
        self.ws = create_connection(
            "ws://%s/ws/console/ext" % host,
            header=["Authorization: Basic " + auth],
            timeout=1.0,
        )
        self.buf = ""          # accumulated guest output
        self.echo_timeout = echo_timeout
        self.quiet = quiet

    def _read(self, timeout):
        """Read one frame within `timeout` seconds; append it. Return True if
        data arrived. A frame is delivered the instant it is available, so a
        caller that re-checks its condition after each _read reacts with no
        added latency."""
        try:
            self.ws.settimeout(max(0.001, timeout))
            data = self.ws.recv()
        except Exception:
            return False
        if not data:
            return False
        text = data.decode("latin-1") if isinstance(data, bytes) else data
        if text:
            self.buf += text
            if not self.quiet:
                sys.stdout.write(text)
                sys.stdout.flush()
        return True

    def expect(self, pattern, timeout=30.0):
        """Wait until pattern (regex) appears in the output. Returns the match."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]   # consume up to the match
                return m
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("expected %r, buffer tail: %r"
                                   % (pattern, self.buf[-200:]))
            self._read(remaining)

    def _send_char(self, ch):
        """Send one byte and return as soon as its echo is seen. The buffer is
        checked before every read, so the next character follows the echo with
        no fixed per-character delay."""
        self.ws.send_binary(ch.encode("latin-1"))
        # CR is normally echoed as CRLF; accept either the char or a newline for it
        wanted = ("\r", "\n") if ch == "\r" else (ch,)
        deadline = time.monotonic() + self.echo_timeout
        while True:
            for w in wanted:
                i = self.buf.find(w)
                if i >= 0:
                    # drop the echoed char so it is not re-matched by a later expect
                    self.buf = self.buf[:i] + self.buf[i + 1:]
                    return True
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False   # never echoed (non-echoing input, or guest not reading)
            self._read(remaining)

    def send(self, line, cr=True, require_echo=True):
        """Type a line echo-driven, then CR. Falls back to a small delay for a
        character the guest does not echo (e.g. password input)."""
        for ch in line:
            if not self._send_char(ch) and require_echo:
                # no echo: pause to let the one-char buffer drain, then continue
                time.sleep(0.05)
        if cr:
            self._send_char("\r")

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


def load_pw(path):
    return open(os.path.expanduser(path)).read().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", nargs="*",
                    help="alternating expect/send strings (expect first)")
    ap.add_argument("-f", "--file", help="script file of 'expect: ...' / 'send: ...' lines")
    ap.add_argument("--host", default=os.environ.get("QBONE_HOST", "qbone"))
    ap.add_argument("--pw-file", default="~/.qbone-pw")
    ap.add_argument("--echo-timeout", type=float, default=2.0,
                    help="seconds to wait for a character's echo (default 2)")
    ap.add_argument("--expect-timeout", type=float, default=60.0)
    ap.add_argument("--interactive", action="store_true",
                    help="after the first expect, relay stdin lines echo-driven")
    args = ap.parse_args()

    con = Console(args.host, load_pw(args.pw_file), echo_timeout=args.echo_timeout)

    # build the step list: (kind, text)
    steps = []
    if args.file:
        for raw in open(args.file):
            raw = raw.rstrip("\n")
            if raw.startswith("expect:"):
                steps.append(("expect", raw[len("expect:"):].strip()))
            elif raw.startswith("send:"):
                steps.append(("send", raw[len("send:"):]))
            # blank/comment lines ignored
    else:
        for i, s in enumerate(args.steps):
            steps.append(("expect" if i % 2 == 0 else "send", s))

    try:
        for kind, text in steps:
            if kind == "expect":
                con.expect(text, timeout=args.expect_timeout)
            else:
                con.send(text)
        if args.interactive:
            for line in sys.stdin:
                con.send(line.rstrip("\n"))
                con.expect(r".", timeout=args.expect_timeout)  # let output settle
    except TimeoutError as e:
        sys.stderr.write("\nconsole_send: timeout: %s\n" % e)
        con.close()
        sys.exit(1)
    con.close()


if __name__ == "__main__":
    main()
