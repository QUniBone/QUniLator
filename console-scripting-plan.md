# Console scripting and session capture — implementation plan

Implements the requirements in
[`docs/planning/console-scripting.md`](docs/planning/console-scripting.md)
([issue #38](https://github.com/QUniBone/QUniLator/issues/38)): one
console-session core serving scripted guest dialogs, timed session recording,
and HTML rendering.

## 1. Shape of the work

Four deliverables, in dependency order:

1. **The harness package** (`console-harness/`, TypeScript) — session core,
   step-file engine, asciicast v3 recorder, renderers, CLI. Host-side only;
   no backend change needed to start.
2. **Backend assists** (C++, `10.05_web`) — the replay-end anchor frame and
   BREAK, two small additions to the console WS contract.
3. **MCP integration** — session tools built on the package;
   `run_xxdp_diagnostic` rebuilt on the core.
4. **Manual capture** — the backend recorder, its API, and the web UI
   (record button, recordings list, embedded player).

## 2. The harness package

New top-level `console-harness/`: an npm package (`qcon`), ESM TypeScript,
mirroring `mcp-server`'s conventions (tsc build, `node --test`, pinned
dependencies). It is self-contained — the MCP server consumes it as a
dependency; nothing in it imports MCP or assumes a QUniLator board beyond the
transport it is given.

```
console-harness/
  src/transport.ts    Transport interface; WS and TCP implementations
  src/session.ts      anchor, output accumulator, input modes, BREAK, events
  src/matcher.ts      stream patterns and screen-state patterns
  src/screen.ts       @xterm/headless wrapper (shared by matcher and renderer)
  src/steps.ts        step-file schema, loader, executor
  src/recording.ts    asciicast v3 writer/reader + annotation sidecar
  src/render/doc.ts   static HTML documentation render
  src/render/player.ts self-contained player page (asciinema-player embed)
  src/cli.ts          qcon run / record / render / break / repl
  test/               mock-transport unit tests, golden render tests
```

Dependencies: `ws`, `@xterm/headless`, `@xterm/addon-serialize`, `yaml`,
`asciinema-player` (bundled into the player page output). CLI binary: `qcon`.

### 2.1 Transport

```ts
interface Transport {
  open(): Promise<void>;
  onData(cb: (bytes: Uint8Array) => void): void;
  onLive(cb: () => void): void;      // replay boundary (see §3.1)
  send(bytes: Uint8Array): void;
  sendBreak(): Promise<void>;        // rejects where unsupported
  close(): void;
}
```

- **`WsTransport`** — a QUniLator console channel: binary frames are the byte
  stream; TEXT frames are OOB control (`{"answerer":true}` today,
  `{"live":true}` from §5.1). `sendBreak()` sends `{"break":true}`.
- **`TcpTransport`** — a raw TCP console (a simh instance's telnet console,
  a ser2net port), which is what makes the package useful outside the
  QUniLator context. `sendBreak()` sends the telnet BREAK command when the
  peer negotiated telnet, and rejects on a raw socket.

Channel selection: `console: auto` resolves the channel from the board —
`GET /api/settings` `external_console.source == ttys2` → `ext`, otherwise the
enabled DL11 at 777560 → `0`, the VAX platform → `vax` — so a script names
the machine, not the wiring. Explicit `ext`/`0`/`1`/`vax`/`tcp:host:port`
override it.

### 2.2 Session

The session owns one open transport for the run, the output accumulator, and
the recorder.

- **Anchor**: matching starts at the replay boundary. With the `{"live":true}`
  frame (§5.1) the boundary is exact; until it ships — and on TCP — the
  fallback is a settle heuristic (anchor after the first idle gap, the
  `consoleRead` approach). Replayed bytes are kept, marked pre-anchor, and
  appear in failure diagnostics as "context before the run".
- **Output accumulator**: bytes since anchor with per-step marks; a step's
  match window is `since(stepMark)`. Diagnostics slice the same buffer.
- **Input modes** per step:
  - `echo` (default) — per character: send, wait for echo progress (printable
    characters received since the send mark, the `runXxdpDiagnostic`
    counting, tolerant of BEL/`^U`/CRLF translation and control-character
    echo like `^C`), resend after `echoTimeout` (default 800 ms), at most
    `maxResend` (default 4) attempts, then the step fails "character not
    echoed". A leading settle delay (default 400 ms) precedes the first
    character.
  - `no-echo` — fixed per-character delay (default 50 ms), single
    transmission, no confirmation. For declared password prompts and
    half-duplex dialogs.
  - `raw` — the bytes in one write, unpaced, for binary protocols.
- **Machine events**: for a QUniLator target the session also holds
  `/ws/events`; `halt`, power loss, or a configurable log pattern becomes a
  deviation the moment it arrives. On TCP targets this input is absent and
  the feature is inert.
- **BREAK**: `session.break()` and a `break: true` step action.
- **Recording**: every event — output bytes, input bytes, BREAK, step
  boundaries, deviations — is timestamped into the recorder as it happens
  (§2.4). A failed run's recording is part of its failure report.

### 2.3 Steps and the step file

YAML, human-edited, one document per script:

```yaml
console: auto
timeout: 30s          # per-step default
deviations:           # checked during every step, fail fast
  - match: "\r\n@"    # the machine fell into ODT
    fail: CPU dropped to the console prompt
  - event: halt
    fail: CPU halted
steps:
  - expect: "login: "
    send: root
  - expect: "Password:"
    send: ${ROOT_PW}      # from --var / environment, kept out of the file
    mode: no-echo
  - expect: "# "
    send: sh /usr/local/install.sh
    timeout: 10m
  - expect:
      - match: "continue? (y/n)"
        send: y
        goto: install-wait     # branching: named steps are jump targets
      - match: "# "
```

- `expect` is a string (fixed text) or `/regex/`, or a list of cases, each
  case `{match, send?, mode?, break?, goto?, fail?, done?}` — the goexpect
  case model and the generalized XXDP answer map.
- `screen:` alongside `match:` matches screen state instead of the stream
  (§2.5): `screen: {row: 23, contains: "MicroEMACS"}` or
  `screen: {contains: ...}` anywhere on the emulated screen.
- Timeouts are per step (`timeout: 90s`), defaulting from the header.
  Deviations listed in the header apply to every step; a step can add its
  own.
- `${NAME}` interpolates from `--var NAME=value` and the environment, so
  passwords and host-specific values stay out of checked-in scripts.
- The executor reports failure as: step name/index, patterns awaited, elapsed
  time, output since the step began, and a bounded pre-step tail — plus the
  path of the recording.

The library API (`new Session(...)`, `session.expect(...)`,
`session.sendLine(...)`) underlies the executor and stays public for
programmatic runs (the MCP tools, ad-hoc scripts).

### 2.4 Recording

Asciicast v3, written incrementally (NDJSON appends, crash-safe):

- header: `{version: 3, term: {cols, rows, type}, timestamp, title, env}` —
  geometry from the script (default 80×24), `title` from the script name.
- events: `o` output, `i` input (as sent, before echo), `m` markers for step
  boundaries (`"step 4: expect login:"`), deviations and BREAK, `x` exit
  status of the run.
- **Annotation sidecar** `<name>.cast.notes.json`: echo spans (which output
  byte ranges are the guest's echo of which input event), step records
  (index, name, outcome, timing), and redaction spans for `no-echo` steps.
  The `.cast` file stays strictly standard so any player takes it; the
  sidecar carries what the format has no field for. `qcon render` reads
  both; a missing sidecar (a board-side manual capture, a foreign cast)
  degrades to heuristic echo correlation.
- `no-echo` input is recorded **redacted by default** (`i` event data
  replaced by `•` of equal length, the true bytes omitted); `--record-secrets`
  keeps them for the rare debugging case.

### 2.5 Screen-state matching

`screen.ts` feeds every output byte into an `@xterm/headless` Terminal and
exposes `rowText(n)`, `find(text|regex)`, `cursor()`. The matcher evaluates
`screen:` conditions against it after each received chunk, the same cadence
as stream patterns. The interpreter instance is per session and is the same
class the renderer uses, so a screen assertion in a script and the rendered
documentation see identical screen state. Unknown escape sequences are
parsed and dropped by xterm's parser; guest emulations beyond the xterm
superset hook in through its `parser.register*Handler` API behind our
`ScreenModel` interface, which is the extension point for a VT52 or plain
teletype personality later.

### 2.6 Rendering

Two outputs from one `.cast` (+ sidecar):

- **`qcon render --mode doc`** — static standalone HTML: the output stream
  replayed through `screen.ts`, snapshotted at step markers
  (`serializeAsHTML`), with operator input rendered as distinct styled lines
  (from `i` events, echo spans folded so typing appears once), step headings
  and elapsed times from the markers. Self-contained file, styles inlined —
  the "installation dialog for the manual" artifact.
- **`qcon render --mode player`** — a single HTML file embedding the
  asciinema-player bundle and the cast data: timed replay, markers as
  chapter navigation, keyboard control. Input display uses the player's
  input-event API with a small overlay (a "keystrokes" strip), since the
  player renders `i` events through its event hooks.

## 3. Backend assists (`10.05_web/2_src`)

### 3.1 Replay-end frame

`console_channel_c::add_client()` sends `{"live":true}` as a TEXT frame after
the ring snapshot and before inserting the client into the live set, under
the same lock — the boundary between replayed and live bytes becomes exact
for every client. The DL11 taps construct their channels without the text
callback today; every channel gets the callback, and the answerer
designation is gated by its own flag instead of the callback's presence.
Clients that ignore TEXT frames (xterm.js console, websocat) are
unaffected.

### 3.2 BREAK

A client sends `{"break":true}` as a TEXT frame on a console channel:

- `/ws/console/ext` — `rs232_c::SetBreak(1)`, 300 ms, `SetBreak(0)` on the
  tx-writer thread (a queued action, so it serializes with pending paced
  bytes).
- `/ws/console/0`, `/1` — the DL11 model receives a break: a new
  `slu_c::receive_break()` sets the framing-error path the model already
  defines (`rcv_fr_err`, null data byte, `rcv_done`), which is the register
  contract of a real received BREAK.
- `/ws/console/vax` — answered with `{"error":"break unsupported"}` until
  the VAX console defines a meaning for it.

`api.md` documents both TEXT-frame directions on the console channels.

## 4. Manual capture (backend + web UI)

- `console_recorder_c`, one per channel, off by default: started/stopped via
  `POST /api/console/<channel>/recording` `{"action":"start"|"stop",
  "name"?}`. While recording, the channel's `append()` (output) and the WS
  input handlers (a new `record_input()` tap on the channel) feed
  timestamped events to an asciicast v3 file under
  `/var/lib/qunilator/recordings/`. Input is recorded from **all** clients —
  that is the point of recording in the backend.
- Size cap (default 16 MB) auto-stops the recording with an `m` marker
  noting truncation; a `recording` field in the `/ws/events` state frames
  drives UI indication.
- `GET /api/recordings` lists (name, channel, start, size, open/closed);
  `GET /api/recordings/<name>` downloads; `DELETE` removes. Recordings live
  outside the images tree and outside configuration snapshots.
- Web UI: a record toggle on the console page with the standing-recording
  indicator, and a recordings list with download, delete, and **Play** —
  a player page embedding asciinema-player (an npm dependency of the
  frontend) fed from `GET /api/recordings/<name>`.
- The board writes plain v3 casts with no sidecar; renders of board
  captures use the heuristic echo correlation (§2.4).

## 5. MCP integration (`mcp-server`)

- `mcp-server` depends on the package (`file:../console-harness`).
- New session tools: `console_session_open` (returns a session id; channel
  resolved as §2.1), `console_expect`, `console_send_line` (mode parameter),
  `console_send_break`, `console_session_close` (returns the recording).
  Sessions time out after idle disuse so an abandoned agent session frees
  its socket. The existing stateless `console_read` / `console_send` /
  `wait_for_console` remain for one-shot use but their descriptions point
  agents at sessions for any dialog.
- `run_xxdp_diagnostic` is rebuilt as a step-file run on the core (the DRS
  answer map becomes expect cases), keeping its tool schema; its bespoke
  console code in `qbone.ts` retires.
- `tools/console_send.py`, `tools/vax-console.mjs`, `tools/odt.py` are
  superseded by `qcon run` / `qcon repl` and removed once the CLI covers
  their uses on the boards.

## 6. Phases

Each phase lands green (build, tests, board validation where hardware is
touched) before the next begins.

1. **Session core + CLI** — package skeleton, transports (WS with settle
   anchor, TCP), input modes, stream matching, deadlines, deviations,
   branching, step files, recorder + sidecar, `qcon run`. Validation: mock
   transport unit tests; on hardware, a 2.11BSD boot-to-login-to-shutdown
   script and an XXDP load-and-run script on `qbone` (unibone for the
   Unibus side later).
2. **Backend assists** — replay-end frame (exact anchor replaces the settle
   heuristic on WS), BREAK on ext and DL11 channels, `api.md`. Validation:
   extended `console_channel_test.cpp` host test (frame ordering, answerer
   flag decoupling); on hardware, BREAK observed by a guest on the real SLU
   and the framing-error registers checked on an emulated DL11.
3. **Screen matching** — `screen.ts`, `screen:` step conditions. Validation:
   unit tests on captured full-screen byte streams (an EDT session, a
   redrawn menu); a live script asserting on a full-screen guest.
4. **Rendering** — `qcon render` doc and player modes. Validation: golden
   HTML tests from checked-in casts; a rendered install session reviewed by
   eye.
5. **MCP sessions** — session tools, `run_xxdp_diagnostic` rebuilt, Python/JS
   one-off tools retired. Validation: mcp-server tests; an end-to-end XXDP
   run via MCP on the board.
6. **Manual capture** — backend recorder + API + web UI record/list/play.
   Validation: host test for the recorder; on the board, a hand-driven
   session recorded from the web console, downloaded, rendered, and the
   web player exercised in a real browser (screenshots per the web-UI
   testing rule).

## 7. Testability

- **Mock transport** (`test/mock-guest.ts`) scripts a guest: echo with
  configurable per-character delay, dropped characters (the no-FIFO fault),
  CRLF translation, control-character echo, no-echo prompts, replay
  prefixes. On it: echo pacing waits on delayed echo and resends only on
  stall; `no-echo` never resends; deadlines fire per step; deviations
  preempt; branching follows cases; the anchor discards replay; the
  recorder's output round-trips through the reader and validates as v3.
- **Golden files**: cast + sidecar → doc HTML compared to checked-in
  snapshots.
- **Host tests** (C++): channel frame ordering and recorder behavior ride
  the existing `10.05_web/tools` harness.
- **CI**: a node job builds and tests `console-harness` (and continues to
  build `mcp-server`); the host-test job picks up the extended channel
  test.

## 8. Decisions

Resolved here (requirements-level decisions are in the requirements doc):

- Package directory `console-harness/`, package and binary name `qcon`,
  consumed by `mcp-server` via a `file:` dependency.
- The replay boundary is an OOB `{"live":true}` TEXT frame in the existing
  control-frame convention; the settle heuristic remains as the fallback for
  TCP targets.
- BREAK crosses as `{"break":true}` client→server TEXT; ext pulses the tty
  via `rs232_c::SetBreak`, the DL11 model takes `receive_break()` through
  its framing-error contract.
- Echo-correlation and redaction spans live in a sidecar file; the `.cast`
  stays strictly standard.
- `no-echo` input records redacted by default.
- Board recordings cap at 16 MB and auto-stop.

Open (settled during implementation):

- The exact YAML schema details (duration syntax, case fields) — fixed by
  the Phase 1 tests.
- Whether `qcon repl` (interactive session with the harness's pacing, for
  exploratory work) lands in Phase 1 or later.
- The keystroke-overlay design on the player page.
