#!/bin/bash
#
# shim-console.sh: run a VAX program on the shimmed core
#
# The seam test of docs/plans/vax-unibus-plan.md stage 0. It assembles a few
# instructions, loads them at address 0 and runs them, and what they do reaches
# every part of the shim that an executing processor uses:
#
#	the loader		places the program in memory
#	instruction execution	MTPR, MFPR and BBC run out of that memory
#	the console		MTPR to TXDB hands a character to the shim's
#				byte channel, which shim_main.c prints
#	the event queue		the console transmitter finishes a character
#				through a scheduled event, and the program's
#				BBC loop waits for the DONE it sets
#	the stop path		HALT ends the run and the batch loop unwinds
#
# The program writes OK and halts. A run that prints it has exercised all five.

set -u

here=$(cd "$(dirname "$0")" && pwd)
shim=$here/../4_deploy/vax780-shim
rundir=$here/../4_deploy/shim-console

if [ ! -x "$shim" ]; then
    echo "$0: $shim is not built - run make in ../2_src" >&2
    exit 2
fi

rm -rf "$rundir"
mkdir -p "$rundir"

# MTPR takes a longword source, so a character above the short-literal range
# needs a full immediate longword. BBC waits for TXCS<7>, the transmitter's DONE.
python3 - "$rundir/console.bin" <<'PROGRAM'
import sys

TXCS, TXDB = 0x22, 0x23
program = bytearray()

def source (value):
    if value <= 0x3F:
        return bytes ([value])                              # short literal
    return b'\x8F' + value.to_bytes (4, 'little')           # immediate longword

def emit (character):
    program.extend (b'\xDA' + source (ord (character)) + bytes ([TXDB]))
    wait = len (program)
    program.extend ([0xDB, TXCS, 0x50])                     # MFPR #TXCS, R0
    program.extend ([0xE1, 0x07, 0x50, 0])                  # BBC #7, R0, wait
    program[-1] = (wait - len (program)) & 0xFF

for character in "OK\r\n":
    emit (character)
program.append (0x00)                                       # HALT

open (sys.argv[1], 'wb').write (bytes (program))
PROGRAM

output=$("$shim" -m 8 -l 30 "$rundir/console.bin" 2>"$rundir/messages")
status=$?

printf '%s' "$output" >"$rundir/console.out"
cat "$rundir/messages"

if [ $status -ne 0 ]; then
    echo "FAIL: the core stopped with status $status"
    exit 1
fi
case $output in
    OK*)
        echo "PASS: the shimmed core ran the program and printed its output"
        ;;
    *)
        echo "FAIL: expected OK on the console, got '$output'"
        exit 1
        ;;
esac
