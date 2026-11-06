# VT100

A DEC VT100 for the board's console channels, opened from the console card's
VT100 button. It is [PCjs](https://www.pcjs.org/machines/dec/vt100/)'s
emulation of the terminal's hardware, an 8080 with the VT100's video and
keyboard, running DEC's VT100 firmware.

| File | What it is |
|---|---|
| `vt100-uncompiled.js` | PCjs's VT100 machine, release 3.01, unchanged. The uncompiled build, because `vt100-console.js` reads the serial port's receive queue by its field names (`abReceived`, `iReceived`), which the compiled build renames |
| `vt100.json` | PCjs's VT100 configuration, with the firmware ROM (8 KB) and the character generator ROM (2 KB) inline. Its SDF connection and its debugger are taken out and its serial port is wired to `qunilator` |
| `vt100.html` | the page: the machine's screen, LEDs and keys |
| `vt100-console.js` | connects the VT100's serial port to the console WebSocket named by `?path=` |
| `vt100-nvr.js` | keeps the terminal's NVR, where SET-UP saves its settings, in the browser's local storage |
| `PCJS-LICENSE.txt` | PCjs's license |

Taken from github.com/jeffpar/pcjs at commit 99e159ba0a6a (2026-11-04),
`machines/dec/vt100/releases/3.01/vt100-uncompiled.js` and
`machines/dec/vt100/vt100.json`.
To update, copy the newer release and regenerate `vt100.json` from PCjs's the
same way: drop the `sdf` and `debugger` entries and set the machine's
`connection` to `serialPort->qunilator` and `autoSave` to false.

The VT100 firmware and character generator ROMs are DEC's, as PCjs distributes
them.

Function keys: F1-F4 are PF1-PF4, F6 BREAK, F7 LINE FEED, F8 NO SCROLL,
F9 SET-UP.

A new terminal starts with DEC's factory settings: 9600 baud, auto XON/XOFF
on and smooth scroll on. Smooth scroll shows a few lines a second, as on the
real terminal; SET-UP B switches it to jump scroll. SHIFT-S in SET-UP saves
the settings into the NVR, which this browser keeps for the board, and the
next VT100 window starts with them.
