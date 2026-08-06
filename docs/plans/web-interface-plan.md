# QBone Web Interface — Implementation Plan

A browser UI for QBone/UniBone, embedded in the `demo` application. It gives
remote access to everything the terminal menu offers for daily operation:
device and parameter management, disk image handling, the PDP-11 serial
console, emulation configurations, and live machine state.

The visual and interaction design is fixed by the clickable mockup in
[`web-interface-mockup.html`](web-interface-mockup.html) — dashboard (tiles, 80×24 console, control row,
RL02 panels, log), devices page generated from parameter metadata, storage,
configurations, and diagnostics pages.

## 1. Architecture

```
Browser (static SPA + xterm.js)
   │  HTTP: static files, REST JSON          WS: /ws/console/<n>  /ws/events
   ▼
civetweb, embedded in `demo` (worker threads)
   │ reads: snapshots under registry lock     │ writes: injected command lines
   ▼                                          ▼
device_c::mydevices / parameter_c        inputline_c queue → menu loop
logger sink / rs232adapter tap / gpios   (single writer, unchanged semantics)
```

Two principles keep the risk low:

- **Reads are snapshots.** Device lists, parameter values and state are
  serialized to JSON under a short lock. No device semantics are duplicated.
- **Writes go through the parameter system.** The menus are one client of
  the parameter API (`param_by_name()->parse()`, `enabled` is itself a
  parameter); the web handlers are another. A single operations mutex
  serializes web requests against menu commands. No menu context, no
  command queue — a write is one direct call.

Device lifetime is decoupled from menu navigation: the device set lives in
`device_configuration_c` (extracted from the devices menu). With `--web`
the application creates it at startup for the process lifetime; the devices
menu borrows it, and quitting the menu keeps it alive. The other hardware
test menus (which reload PRU code) are unavailable while it exists.

The server is compile-time optional (`-DWEBUI`); without the flag the demo
binary is unchanged.

## 2. Directory layout

```
10.05_web/
  README.md
  docs/
    plan.md              this document
    (the mockup moved to docs/plans/web-interface-mockup.html)
    api.md               REST/WS reference (written with phase 1)
  2_src/                 C++ server sources, compiled into demo
    webserver.hpp/.cpp   webserver_c: civetweb lifecycle, routing, static files
    webapi.cpp           REST handlers, JSON serialization
    webevents.cpp        event hub → /ws/events broadcast
    webconsole.cpp       DL11 byte streams ↔ /ws/console/<n>
  3_frontend/            SPA, served as-is (no build step)
    index.html
    css/tokens.css       design tokens (single source, from the mockup)
    css/app.css          component styles, reference tokens only
    js/                  ES modules: store.js, api.js, pages/*.js
    vendor/xterm.js      vendored terminal (self-contained, no CDN)
91_3rd_party/
  civetweb/              embedded HTTP/WebSocket server (MIT, C, static-linkable)
  picojson/              header-only JSON reader/writer
```

The frontend ships inside the git tree, so `update-code.sh` distributes it
with no packaging changes. `qunibone-platform.sh` needs no change.

## 3. Backend

### 3.1 Server (`webserver_c`)

- civetweb with a small worker pool (4 threads), started from
  `application_c::run()` when `--web [port]` is given (default port 80; the
  demo runs as root). With `--web`, the application enters the devices menu
  automatically after startup, so the API always finds live devices.
- Serves `10.05_web/3_frontend` as document root; JSON API under `/api/`,
  WebSockets under `/ws/`.
- `webserver_c` is a `logsource_c` like every other subsystem; its port and
  bind address come from the command line, an optional password from
  `qunibone-platform.env` (`WEBUI_PASSWORD`, HTTP basic auth; unset = open,
  matching the bench-LAN trust model).
- civetweb compiles as C99 with jessie's gcc and links into the existing
  fully-static binary.

### 3.2 Device configuration

`device_configuration_c` (10.03_app_demo) holds the complete device set —
construction and destruction order taken from the devices menu, which now
borrows the configuration instead of owning it when it already exists.
`application_c::devices_startup()/devices_shutdown()` bring up PRU emulation
code plus the device set; web mode calls them at startup and exit.

### 3.3 Writes

`device_configuration_c::operations_mutex` serializes device operations:
the devices menu takes it around each command, every web write handler takes
it around its parameter call or bus action. Parameter validation errors
(`bad_parameter`) map to HTTP 422 with the device's message.

### 3.4 Snapshots and locking

- A registry mutex guards `device_c::mydevices` (taken in the `device_c`
  constructor/destructor and by the snapshot code). Devices are created and
  destroyed only on menu entry/exit, so contention is nil.
- `GET /api/devices` walks the registry and each device's `parameter` vector,
  emitting per parameter: `name`, `shortname`, `type` (string/bool/unsigned/
  unsigned64/double), `value` (via `render()`), `readonly`, `unit`, `info`,
  and for unsigned parameters `base` and `bitwidth` — everything the frontend
  needs to generate octal-aware forms. Enum-like string parameters
  (`shared_filesystem`) get an optional value list added to `parameter_c` so
  they render as dropdowns.
- `GET /api/state` reports platform, address width, menu-active flag, bus
  signals (DCOK/POK, HALT), LEDs, DIP switches and the running configuration.

### 3.5 Event push (`/ws/events`)

One multiplexed JSON WebSocket keeps every open page live:

- **Parameter changes**: a global observer hook in `parameter_c::set()`
  (fires after commit) queues `{"t":"param","dev":…,"param":…,"value":…}`.
  This covers enable/disable, image attach, panel lamp/switch state — they
  are all parameters.
- **Log**: a sink callback in `logger_c::vlog()` forwards filtered messages
  as `{"t":"log",…}`.
- **Hardware**: a 10 Hz poll (matching the panel worker cadence) publishes
  LED, DIP and bus-signal state on change.

The event hub serializes on its own queue; producers never block on slow
WebSocket clients.

### 3.6 Console streams (`/ws/console/0`, `/ws/console/1`)

Byte-transparent binary WebSockets bridged to the two DL11 `slu_c` units via
`rs232adapter_c`:

- Transmit direction: a tap on the adapter's xmt path copies every byte to
  connected WebSocket clients (the physical UART stays attached — the web
  terminal is a parallel tap).
- Receive direction: client bytes are injected through the adapter's rcv
  stream under its existing mutex, exactly as the `dl11 rcv` menu command
  does.
- No echo, no line discipline, all 256 byte values pass through — the
  terminal emulation lives entirely in the browser. This finishes what the
  commented-out `ip_host`/`ip_port` parameters in `dl11w.hpp` started.

### 3.7 REST API

| Method & path | Purpose |
|---|---|
| `GET /api/state` | platform, bus, LEDs, DIP, running config |
| `GET /api/devices` | full device/parameter snapshot (3.4) |
| `PUT /api/devices/<dev>/params/<param>` `{"value":…}` | set a parameter (`enabled` included) |
| `POST /api/control` `{"action":"init"\|"powercycle"\|"halt"\|"continue"}` | bus actions |
| `GET /api/images` | image files in `$QUNILATOR_DIR/images`: name, size, mtime, attached drives |
| `POST /api/images` | upload (multipart) into the images directory |
| `GET /api/images/<name>` | download |
| `GET /api/configs` | saved configurations: name, mtime, enabled devices |
| `GET /api/configs/<name>` | full snapshot content |
| `PUT /api/configs/<name>` | save the current device setup under `<name>` |
| `POST /api/configs/<name>/apply` | restore a snapshot, returns rejections |
| `DELETE /api/configs/<name>` | remove a snapshot |

Attach/detach and enable/disable are parameter writes; the Control row maps
to `/api/control`. A configuration is a JSON snapshot of the device setup —
every device's enabled state and writable parameter values, stored in
`$QUNILATOR_DIR/configs/<name>.json`. Snapshots are taken from and applied to
the live parameter system, so a saved configuration reproduces exactly what
the devices and storage pages show.

### 3.8 Core-code touch points

Deliberately small, upstream-friendly diffs:

| File | Change |
|---|---|
| `10.03_app_demo/2_src/menu_devices.cpp` | device set extracted to `device_configuration_c`, menu borrows an existing set, operations mutex per command |
| `10.03_app_demo/2_src/application.*` | `--web` option, server + device-set startup, hardware test menus blocked while the set exists |
| `10.01_base/2_src/arm/parameter.*` | post-commit observer hook, optional enum value list |
| `10.01_base/2_src/arm/device.*` | registry mutex |
| `90_common/src/logger.*` | sink callback |
| `10.02_devices/2_src/rs232adapter.*` | xmt tap for WebSocket clients |
| `10.03_app_demo/2_src/makefile*` | `WEBUI` flag, new sources, civetweb |

Everything else is new code under `10.05_web/2_src`.

## 4. Frontend

- **Stack**: vanilla ES modules, served raw — no bundler, no node toolchain,
  nothing to install on the BBB or for contributors. The only vendored
  dependency is xterm.js.
- **Design system**: `css/tokens.css` carries the design-token block from the
  mockup verbatim (light/dark via `prefers-color-scheme` +
  `data-theme` overrides); `app.css` references tokens only. Restyling stays
  a one-file operation.
- **State**: `store.js` holds the device/state model, hydrated from
  `GET /api/state` + `GET /api/devices` on load and patched live from
  `/ws/events`. Pages re-render from the store; user actions call `api.js`
  (`POST /api/command` etc.) and rely on the event echo for the visible state
  change — the UI never assumes success.
- **Pages** (1:1 with the mockup):
  - *Dashboard*: tiles (configuration, enabled-device chips, bus chips, LEDs,
    DIP), centered 80×24 console, Control row (INIT/HALT/continue + confirmed
    power cycle), RL02 panels with original cap colors/legends and
    LOAD-lit-when-spun-down behavior, log tail.
  - *Devices*: cards generated from parameter metadata; octal `o` prefix,
    units, help texts, read-only rendering, enable toggles.
  - *Storage*: image table with attach/detach/upload/download, shared
    directories.
  - *Configurations*: script list with `.cmd` viewer and run-as-job.
  - *Diagnostics*: level-filtered live log.
- **Terminal**: xterm.js, fixed 80×24, no local echo, raw key forwarding
  (the mockup's contract), binary WebSocket transport. The RL02 panel lamps
  and LEDs update from `/ws/events` parameter messages.

## 5. Build & deployment

- `makefile_q`/`makefile_u`: `WEBUI=1` (default on) adds `10.05_web/2_src/*`
  and `91_3rd_party/civetweb/civetweb.c` to the objects, `-DWEBUI`, and the
  include paths. Static linking is unaffected; expected binary growth is
  ~1 MB.
- `compile.sh` unchanged. `update-code.sh` distributes frontend + server
  sources via the existing GitHub tarball path.
- Operation: `demo --web` for manual use; adding `--web` to the
  `5_applications` scripts / `autostart.sh` makes headless boot fully
  web-controlled (documented, done in phase 4).

## 6. Phases

| Phase | Deliverable | Scope |
|---|---|---|
| **0 — Scaffolding** | `demo --web` serves the static frontend on the BBB | vendored civetweb + picojson, `webserver_c`, makefile integration, directory skeleton |
| **1 — Remote control** | Devices page fully functional | inputline queue + select loop, cout tee, `/api/command`, `/api/devices`, `/api/state`, generated device forms |
| **2 — Console & live state** | Dashboard fully functional | rs232adapter tap, `/ws/console`, xterm.js; parameter observer, logger sink, `/ws/events`; tiles, Control row, RL02 panels, log live |
| **3 — Storage & configurations** | Storage + Configurations pages | image list/upload/download/attach, config list/run, jobs API |
| **4 — Polish** | Production niceties | basic auth, shared-dir browser, autostart docs, `api.md`, upstream PR preparation |

Each phase is independently shippable; phase 1 already replaces
`ssh + screen` for routine operation.

## 7. Testing

- **Frontend without hardware**: a fixture server (small script under
  `10.05_web/tools/`, runs on the development PC) serves the SPA plus canned
  API/WS responses — UI work needs no BBB.
- **Backend units**: JSON serialization of the parameter snapshot is testable
  host-side (the parameter classes have no hardware dependency).
- **On-target smoke test**: a curl script exercises every REST endpoint and
  one WebSocket round-trip against a running QBone; the RT-11 boot via web
  console is the end-to-end check.
- **Regression guard**: build without `WEBUI` must stay byte-identical in
  behavior; `--cmdfile` scripts must run unchanged with the new readline.

## 8. Upstream strategy

Development happens on one feature branch in a fork of `j-hoppe/QUniBone`.
The branch lands upstream as three pull requests — core enablers (registry
mutex, parameter/logger/rs232adapter observer hooks, device-set
extraction), the web interface itself, and cross-compilation support.
