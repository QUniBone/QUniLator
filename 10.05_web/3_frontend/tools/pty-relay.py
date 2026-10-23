#!/usr/bin/env python3
# Run a command on a pseudo-terminal and relay its bytes: stdin to the
# terminal, the terminal to stdout. The transfer test talks to a receiving
# program through this, as the browser talks to a guest through its console.
import os, pty, select, sys

pid, fd = pty.fork()
if pid == 0:
    os.execvp(sys.argv[1], sys.argv[1:])
inp = sys.stdin.fileno()
out = sys.stdout.fileno()
open_in = True
while True:
    rl = [fd] + ([inp] if open_in else [])
    r, _, _ = select.select(rl, [], [], 0.5)
    if fd in r:
        try:
            data = os.read(fd, 4096)
        except OSError:
            break
        if not data:
            break
        os.write(out, data)
    if inp in r:
        data = os.read(inp, 4096)
        if not data:
            open_in = False
        else:
            os.write(fd, data)
_, status = os.waitpid(pid, 0)
sys.exit(os.waitstatus_to_exitcode(status))
