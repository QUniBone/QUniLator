# Console scripting and session capture

**Status:** Ready — implementation plan drafted in
[`console-scripting-plan.md`](../../console-scripting-plan.md)
([issue #38](https://github.com/QUniBone/QUniLator/issues/38)).

Driving a guest over the console is the only way to automate a boot, an install
or a diagnostic run. Every attempt so far was written from scratch and failed in
the same handful of places; this defines the one harness that gets those right
once, plus the session recording and rendering that grow out of the same byte
streams.

Three capabilities, sharing one console-session core:

1. **Scripting** — expect/send dialogs against a guest, with echo-paced input,
   per-step deadlines, fast deviation detection, and branching.
2. **Recording** — a timed, direction-tagged log of every session: scripted runs
   automatically, and manual (human-driven) sessions on request, so a hand-done
   installation can be captured for replay or documentation.
3. **Rendering** — a recorded session rendered to HTML with operator input
   visually distinct from guest output, interpreting the guest's terminal escape
   sequences (ANSI/VT100 first, extensible to other emulations).

## Current state

### The transport

Console I/O crosses the board as byte-transparent binary WebSocket frames:
`/ws/console/ext` (the real console SLU on `/dev/ttyS2`), `/ws/console/0` / `/1`
(the emulated DL11s), `/ws/console/vax` (the emulated VAX's own console), and
`/ws/serial/<dev>/<line>` for mux lines in `websocket` mode. Each channel is a
`console_channel_c`: a 256 KB in-memory ring of raw output bytes, replayed in
full to every client on connect, then the live stream
([console.md](console.md)).

Facts of that transport that shape this design:

- **The ring holds output only, with no timestamps and no sequence numbers.**
  Client input bypasses the channel object: the WS data handlers write it
  straight to the tty (`webconsole_ext.cpp`) or inject it into the DL11's
  receive stream (`webconsole.cpp`). Timing and the input direction exist only
  at the moment the bytes pass through the backend — nothing retains them.
- **Replay on connect** means a reader that matches on everything it receives
  matches a prompt that scrolled past minutes ago. Live reading must be
  anchored at the connect point.
- **The external bridge already paces the wire**: client→tty bytes are queued
  and dripped at 5 ms/char (`TX_PACE_MS`), which keeps the UART itself from
  overrunning. The remaining loss happens one level up — a byte arriving while
  the guest program is between printing its prompt and issuing its read is
  gone, because the SLU has no receive FIFO. Only echo observation catches
  that.
- The channel picks one client as the terminal "answerer" for guest
  identification queries, so mirrored consoles do not all reply.

### Prior attempts in this tree

Each solved part of the problem; the harness supersedes all of them:

- `mcp-server/src/qbone.ts` `runXxdpDiagnostic` — the most complete: config
  setup, boot probing, echo-confirmed per-character send with positional echo
  tracking (a delayed echo waits, a stalled one resends), prompt/answer maps,
  per-phase timeouts, fail markers, a transcript on failure.
- `tools/console_send.py` — echo-driven send, expect:/send: step files, an
  interactive mode.
- `tools/vax-console.mjs` — expect/send/`--send-hidden` (delay-paced input for
  a password that will not echo).
- `tools/odt.py` — blind 50 ms/char pacing; predates echo observation.
- The MCP tools `console_send` / `console_read` / `wait_for_console` — each
  call opens a fresh WebSocket, receives the full replay, and closes; there is
  no session, no anchor, and no pacing in `console_send`.

### Console selection today

There is no single "which channel is the console" field yet. A caller derives
it: `GET /api/settings` reports `external_console.source` (`ttys2` →
channel `ext`); otherwise the enabled DL11 at 777560 is the console (channel
`0`); the VAX platform talks on `vax`. The planned `console_type` setting
([console.md](console.md) §4) would make this one lookup.

## Requirements

### The session

- A script run holds **one persistent connection per channel** for its whole
  duration. Opening a connection per step forfeits the anchor and re-receives
  the replay.
- **Reading is anchored at connect**: the replayed ring is discarded for
  matching purposes (it may still be kept for diagnostics as "context before
  the run"). Every step then matches only output produced after the step
  began.
- The **channel is derived from the machine's configuration** (the settings
  lookup above); a script names the machine, not the channel. An explicit
  channel override remains possible for mux lines.
- The session watches **`/ws/events` in parallel**: a CPU halt, power change or
  relevant log event during a step is a deviation the script reacts to
  immediately — a capability none of the general-purpose expect tools have,
  and a main reason the harness is ours.

### Input: echo-observed pacing

Echo observation is the pacing mechanism: send one character, confirm the
guest echoed it, send the next. A full-duplex guest echoes a character only
after its console driver has read it out of the SLU, so the sender can never
outrun the one-byte receive buffer. The confirmation logic must be prepared
for echoes that are not byte-identical mirrors:

- **Control characters interleaved with or substituted for the echo**: BEL and
  `^U` on a line the guest rejected, CR→CRLF translation, a control character
  echoed as a two-character printable (`^C`), backspace-space-backspace on
  erase. Confirmation therefore counts *echo progress* (printable advance
  since the send mark), rather than comparing bytes literally — the approach
  proven in `runXxdpDiagnostic`.
- **Deliberate non-echo**: a password prompt echoes nothing. A "no echo →
  resend" retry would type the password several times over, so resend-on-stall
  is safe only where echo is expected. Input therefore has explicit per-step
  modes:
  - **echo-confirmed** (default) — paced on the echo, with bounded resend on a
    genuine stall (the dropped-character case);
  - **delay-paced** — fixed per-character delay, no confirmation, no resend;
    for declared no-echo steps (passwords) and half-duplex dialogs;
  - **raw** — bytes sent as-is for non-terminal protocols (a boot loader
    expecting a binary record).
- **Non-printable input**: control characters, `^C`, and escape sequences
  must be expressible in a script step.
- **BREAK**: a script step (and a manual client) can assert a line BREAK.
  BREAK is a line condition, not a byte, so it crosses the WebSocket as an
  out-of-band control frame — the channels already carry OOB text frames for
  the answerer designation. The external bridge asserts it on the tty
  (`tcsendbreak`); an emulated DL11 presents the received-break condition its
  registers define.
- Echo-confirmed steps still start with a **settle delay** before the first
  character: the first byte after a prompt is the one the guest is most likely
  not yet reading for.

### Steps, deadlines, deviations

- A step is: *expect* (one or more patterns) → *respond* (input in one of the
  modes above). Patterns match against the output accumulated **since the step
  started**, not the whole session.
- **Per-step deadline**, easily set per step, with a script-level default. A
  step that passes its deadline fails fast; nothing waits on a global timeout
  to notice.
- **Deviation detection ahead of the deadline**: a script declares global
  patterns (the `expect_before` idea) that fire on any step — error markers
  (`?`, `FATAL`, a panic, the ODT `@` prompt, a bus timeout message), plus the
  `/ws/events` conditions above. A deviation fails the step (or branches)
  the moment it appears, not when the clock runs out.
- **Branching**: a step may name several expected patterns, each with its own
  response — answer an unexpected-but-known prompt, jump to a label, or fail.
  This is the generalization of the XXDP answer map.
- **Screen-state matching**: a full-screen guest (EDT, vi, a menu-driven
  installer) redraws in place, and the byte stream then carries cursor motion
  rather than appended lines. A step may therefore match against the emulated
  screen content — a row, a region, the cursor position — instead of the raw
  stream. The session core runs the same terminal interpreter the renderer
  uses; stream matching remains the default for line-oriented dialogs.
- **Failure diagnostic**: a failed step reports the step's name/index, which
  patterns it was waiting for, the elapsed time, and the console output from
  the start of the step to the point it gave up — plus a bounded tail of
  context from before the step. A bare timeout with no output is not an
  acceptable failure report.

### Recording

- **Every scripted run records itself** — the recording is the run's log and
  the failure diagnostic's source. Events carry timestamps and direction
  (input vs output); the harness adds step-boundary markers so a rendered
  session shows where each step began and what it waited for.
- **Manual capture**: a human-driven session (web UI xterm, or any client) can
  be recorded the same way, so a hand-performed installation becomes a timed
  artifact. Input arrives from any of several concurrent clients and is only
  visible where it converges — the backend bridge — so manual capture is a
  **backend** feature: the channel gains a recording tap seeing both
  directions, started and stopped over the API, writing to a bounded file
  under the state directory.
- One **format** for both producers, so one renderer serves both. The format
  must carry: per-event timestamps, direction, raw bytes (the stream is not
  UTF-8 text), session metadata (channel, machine configuration, start time),
  and annotation markers. The asciicast format (asciinema) is the candidate to
  beat — see prior art.
- Recording is raw: no terminal interpretation at capture time. Interpretation
  belongs to rendering, so a recording outlives today's renderer.

### Rendering

- A recorded session renders to **standalone HTML** with operator input
  visually distinct from guest output, suitable for documentation ("this is
  the install dialog, these are the answers") — and, where useful, a timed
  replay (player-style) view.
- The renderer interprets the guest's escape sequences through a **terminal
  emulation layer** — ANSI/VT100 first, **extensible** to others; VT52 matters
  for RT-11/RSX-era software, and a plain teletype mode for ODT/XXDP output.
- **Echo correlation**: typed input appears twice in a session — as input
  events and again as the guest's echo in the output stream. The renderer must
  present typing once, distinctly. The echo-confirmed sender knows exactly
  which output bytes were echoes of its input; the recording should carry that
  correlation rather than making the renderer guess. For manual capture the
  correlation is heuristic (match input events against closely-following
  identical output) — accepted as best-effort, with non-echoing input
  (passwords) rendered from the input events alone, and redactable.

### Consumers

The core is a **standalone package** — the session library, a CLI runner, and
the declarative step-file format — with no MCP dependency. It serves:

1. **The CLI runner** — checked-in, human-editable scripts (boot regression
   tests, install automation), runnable from a workstation or CI wherever a
   board is reachable.
2. **The MCP server** — session-based tools (open a session, expect, send,
   close-with-recording) built on the library, replacing today's stateless
   per-call console tools; `run_xxdp_diagnostic` rebuilt on the core.
3. **Manual capture** — the backend recording tap plus a web UI affordance.

## Prior art

Surveyed 2026-08: the expect family (Tcl expect, pexpect, google/goexpect,
Netflix/go-expect, ActiveState/termtest, the JS ecosystem), SIMH's built-in
EXPECT/SEND, the recording formats (asciicast v2/v3, ttyrec, util-linux
`script --log-timing`), the players and HTML renderers, and headless terminal
emulators. Verdict per component:

### Scripting engine: build, borrowing the vocabulary

No surveyed tool implements **wait-for-echo pacing**. Everything that paces at
all paces by time: Tcl expect's `send -s`/`send -h`, pexpect's
`delaybeforesend`, SIMH's per-character `DELAY=` (in simulated instructions).
Echo-confirmed send is a custom loop in any of them, and it is the heart of
our problem — so the framework brings little, and what remains (pattern wait,
timeout, transcript) is small.

Driving our transport is the second disqualifier. The console is a WebSocket
byte stream; the tools that accept an arbitrary stream are pexpect
(`fdspawn`/`SocketSpawn`, needs a WS-to-fd bridge), Tcl expect (`expect -open`
on a Tcl channel), and google/goexpect (`SpawnGeneric` — archived since 2023).
The JavaScript side has **no maintained expect library at all**; node-pty,
@microsoft/tui-test (since renamed shell-use) and the Go terminal-test
frameworks all spawn processes on a pty. And none of them can watch our
`/ws/events` machine state alongside the console.

So the session core is ours — the standalone package named under Consumers —
adopting the proven **semantics**:

- pexpect's `before`/`after`/`match` — a step's diagnostic is exactly the
  output between the previous match and this one.
- Tcl expect's `expect_before`/`expect_after` — the global deviation patterns.
- goexpect's batch **cases** (pattern → response → continue/terminate tag) and
  SIMH's expect-rule set — the branching model, and the shape a declarative
  step file would take.
- pexpect's `waitnoecho()` — detecting the guest turning echo off, a useful
  cross-check at a declared no-echo step.

The domain precedents validate the architecture rather than supply code: SIMH
drives its test suites with in-simulator `EXPECT "…" / SEND / GO` chains
(halting simulated time on match — a luxury an external harness replaces with
per-step buffering and deadlines); the ITS reconstruction builds a whole OS
under Tcl expect; TOPS-20 builds under pexpect; QEMU's and LAVA's board-farm
pattern is pexpect over a serial socket with per-step timeouts — a byte
stream, not a pty, exactly our shape.

### Recording format: adopt asciicast v3

[asciicast v3](https://docs.asciinema.org/manual/asciicast/v3/) (asciinema
CLI ≥ 3.0, 2025) is the one established format carrying everything the
requirements name in a single self-describing NDJSON file: a header with
terminal geometry/type/theme and session metadata, then events
`[interval, code, data]` with millisecond relative timing — `o` output, **`i`
input**, `m` marker, `r` resize, `x` exit status. Input capture is an
established mode (`--capture-input`), with the documented caveat that
passwords land in the file — our no-echo steps mark spans for redaction. The
`m` marker events carry the harness's step boundaries; the echo-correlation
spans ride in a harness-namespaced annotation (marker labels or a sidecar —
a design detail for the plan). util-linux `script --log-timing` (advanced
format) records the same information but split across files with no tooling
beyond `scriptreplay`; ttyrec carries output only. We write the format
ourselves (a few dozen lines), so the GPL-3.0 of the asciinema CLI is not
inherited.

### Timed replay: adopt asciinema-player

[asciinema-player](https://github.com/asciinema/asciinema-player)
(Apache-2.0, active) self-hosts as a static JS+CSS bundle — servable by the
board's web UI or embedded in generated documentation. It plays v2/v3 and
ttyrec, and its markers give pause-on-step navigation. It renders `i` events
**not at all** by default, dispatching them through its JS event API — the
operator-input display layer is ours on top, which suits us: it is where the
echo-correlated presentation (requirement above) lives anyway.

### Static HTML and terminal interpretation: build a thin renderer on @xterm/headless

The existing ANSI-to-HTML converters fail the requirements: `aha` and
`ansi2html` translate colors but not cursor motion (a redrawn line renders as
garbage), and buildkite/terminal-to-html interprets the stream correctly but
emits only the final screen — timing, steps and input distinction are gone.
A documentation-grade render is therefore ours: feed the output stream
through a terminal emulator, snapshot at step boundaries, and emit styled
HTML with the input annotations.

For the emulator, [@xterm/headless](https://www.npmjs.com/package/@xterm/headless)
(MIT, actively maintained, TypeScript) is the fit: DOM-free `write()` plus a
full buffer/cell read API, and `@xterm/addon-serialize` emits the current
screen as HTML directly. It matches the stack — xterm.js already renders the
live console in the frontend, so recorded and live sessions interpret bytes
identically. Its emulation target is the xterm superset (covers ANSI/VT100);
it has no strict VT52/legacy personality, and unknown sequences are parsed
and dropped. Extensibility to other emulations (requirement) is a swappable
interpreter interface, with xterm's `parser.registerCsiHandler`-family hooks
as the first extension point and pyte (Python, LGPL) or asciinema's avt
(Rust) as alternatives should a renderer outside the TS stack emerge.

### Licence note

Adopted pieces: asciinema-player Apache-2.0, @xterm/headless MIT, pexpect-
and expect-derived semantics (no code). The GPL-3.0 asciinema CLI/agg and
LGPL pyte/ansi2html are not linked or shipped.

## Decisions

Resolved:

- **The core is a standalone package** — session library + CLI runner + step-
  file format — with no MCP dependency; the MCP server is one consumer of it.
  TypeScript is the working language choice: the core is custom code in any
  language (no adoptable engine exists), and TypeScript keeps one
  byte-interpretation stack across the live console (xterm.js), the session
  core, and the renderer (@xterm/headless).
- **Scripts are declarative step files**, human-readable and human-editable,
  executed by the library; the library API stays available for runs that need
  programmatic control. The exact syntax belongs to the implementation plan
  (the `console_send.py` expect:/send: lineage is the starting shape).
- **Screen-state matching is in scope** (requirement above).
- **Anchoring is client-side**: the session discards the replay at connect;
  the ring gains no sequence numbers. A session whose connection drops fails
  and is re-run rather than resumed.
- **BREAK support is required** (requirement above), carried as an
  out-of-band control frame.
- **The renderer is standalone and integrated.** It is a tool in the harness
  package consuming any asciicast v3 file, with no board dependency — usable
  entirely outside the QUniLator context. The board's web UI integrates by
  listing recordings and embedding the self-hosted asciinema-player for timed
  replay; the static-HTML documentation render runs wherever the CLI does.

- **Recording is explicit start/stop, default off**, with a size cap and
  auto-stop; scripted runs always record on the harness side (the recording
  is part of the run's output on the workstation, not the board). The case
  against always-on capture on the board: input events capture passwords, so
  a standing file of them on the appliance is a liability; continuous writes
  wear the eMMC/SD; and the 256 KB ring already answers "what just happened"
  for casual look-back. An always-on rolling mode can be added later as a
  board setting if practice shows the need.

## References

- [Issue #38 — a reliable harness for scripting guest operating systems over
  the console](https://github.com/QUniBone/QUniLator/issues/38)
- [console.md](console.md) — channel/ring/replay requirements
- [console-plan.md](../../console-plan.md) — `console_channel_c` design
- [serial-ports.md](serial-ports.md) — mux lines carried on `/ws/serial/…`
- [mcp-server.md](mcp-server.md) — the MCP server the session tools extend
