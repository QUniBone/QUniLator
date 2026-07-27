# Plan for `bigger-issues.md`

Implementation plan derived from `docs/bigger-issues.md`. Each wishlist item is
mapped to concrete work with the files it touches, and marked with what already
exists so nothing is rebuilt needlessly. Decisions taken (from review) are
recorded per area; a few small assumptions are called out inline.

Areas are independent enough to build in any order, with two soft dependencies:
the dashboard's per-config layout and the dashboard's config-name/title display
both build on the Configuration work, so Configuration is the natural first
foundation even though no order is committed.

Decisions carried throughout:

- **Dashboard layout is per-config**, stored in `configs/<name>.json`.
- **DIP-switch value selects the auto-loaded config, read at power-on**; each
  config self-describes its DIP value; no match loads an empty passive config.
  This replaces the `settings.json` default-config pointer.
- **TTY backends** get a polymorphic transport abstraction plus a new websocket
  backend.
- **RL** work audits and completes the existing RLV11/RLV12 subclasses.
- **Diagnostics log** gets a disk journal and a paginated newest-first REST feed.
- **Dashboard grid** is a fixed column/row grid; widgets snap to cells.

---

## 1. Configuration (foundation)

Files: `10.05_web/3_frontend/src/components/Configs.ts`,
`.../src/lib/devmodel.ts`, `.../src/types.ts`, `.../src/api.ts`;
`10.05_web/2_src/webconfigs.{cpp,hpp}`, `.../websettings.{cpp,hpp}`,
`.../webevents.cpp`, `.../webapi.cpp`; `10.05_web/docs/api.md`.

### 1.1 Human-friendly, editable configuration title
- Add a `title` field to the config document (`webconfigs.cpp` load/save,
  default = the name). Keep the file name as the stable identity; the title is
  free-text and editable in place.
- New endpoint or extend the existing config-metadata write path; document in
  `api.md`.
- Frontend: render/edit the title in the `Detail` header (`Configs.ts:377-382`),
  keep Rename for the identity name.
- *Assumption:* the title is displayed prominently and the mono name becomes a
  secondary identifier.

### 1.2 Add-device picker lists each type once
- The picker currently offers the same device type once per possible bus
  address. Change it to one entry per device *type*; the address is chosen
  afterward via the address dropdown (see 1.3). Touches how `flatDevices()` /
  the registry feeds the add-device list (`devmodel.ts`, `Configs.ts` add flow).
- Needs a look at how the registry pre-instantiates devices to confirm whether
  the collapse is purely presentational or requires instantiate-on-add.

### 1.3 Address / IRQ as dropdowns with conflict checking
- Replace the octal free-text inputs for address/vector/IRQ with dropdowns of
  the legal values per device (`paramControl`, `Configs.ts:152-158`).
- Add conflict checking as devices are configured: detect address-window and
  vector/IRQ overlaps across enabled devices and flag them in the UI. No such
  check exists today — decide whether validation lives in the frontend, the
  backend (`webconfigs.cpp`), or both; backend is authoritative.

### 1.4 On/off as a checkbox
- Replace the Chip+Toggle pairing (`Configs.ts:208-209`) with a plain checkbox
  for the enabled state.

### 1.5 Terminology cleanup
- Drop "stored"; keep "current" as a chip; remove "live".
  Touches the detail-header kicker (`Configs.ts:377`) and the master-list chips
  (`Configs.ts:434-436`).
- Remove the staging text at `Configs.ts:398-403`
  ("Edits are staged here and reach nothing until you Save." and its
  current-config counterpart).

### 1.6 DIP-switch-selected configuration (replaces default-config)
- Add a `dip_value` field to the config document (which DIP-switch setting
  selects it). 4 DIP switches → 16 possible values.
- On power-on, read the DIP value and load the matching config; if none matches,
  load an empty passive-on-bus config. Read at power-on only; changing DIPs
  mid-run takes effect on the next powercycle.
- Rework `webconfigs_startup()` (`webconfigs.hpp:26`) and the startup selection
  chain (currently `--config` override → `settings.json` default → bundled
  fallback) to select by DIP value. Remove the default-config pointer
  (`websettings_default_config()` / `..._set_default_config()`,
  `webconfigs_set_default`, and the `PUT /api/configs/<name>/default` endpoint).
- The DIP value is already polled and published (`webevents.cpp:239`,
  `switches[]`); reuse that read at power-on.
- Frontend: replace the "default" chip/affordance with a per-config DIP-value
  selector; document the new behaviour in `api.md`.

---

## 2. Dashboard

Files: `10.05_web/3_frontend/src/components/Dashboard.ts`, `.../widgets.ts`,
`.../Shell.ts`, `.../store.ts`, `.../styles.css`, `.../types.ts`,
`.../lib/devmodel.ts`; config plumbing from area 1.

### 2.1 Fixed column/row grid, widgets snap to cells
- Replace the auto-fill `.widget-grid` (`styles.css:699`) with a fixed-dimension
  grid (e.g. 12 columns). Each widget occupies a cell/span; position = grid
  coordinates.
- *Assumption:* start at 12 columns with an auto row height; refine after seeing
  it on the real console width.

### 2.2 Drag-and-drop with an edit mode; positions stored per-config
- Add a dashboard **edit mode** toggle: in edit mode widgets can be dragged
  between grid cells and hidden/shown via an eye icon; hidden widgets are visible
  only in edit mode. Save / revert commits or discards.
- Persist layout (per-widget grid position + hidden flag) into the config
  document (area 1 schema addition), plumbed through `api.ts` and
  `webconfigs.cpp`. Switching configs switches layout.
- `Widgets()` (`widgets.ts:227-239`) consumes the stored layout for placement,
  ordering, and visibility instead of raw enumeration order.

### 2.3 Current configuration name + title on the dashboard, cog to config
- Show the current config's title + name on the dashboard (data already in
  `store.configCurrent`, `store.ts:24`). Add a cog/gear icon linking to
  `/config` (no such icon exists today; add to `Shell.ts` topbar or the
  dashboard header).

### 2.4 Console gets a "console" card
- Wrap `TerminalHost` (`Dashboard.ts:123-130`) in the same card shell as the
  other widgets so the console is a first-class, placeable/hideable card in the
  grid.

### 2.5 Front-panel DIP switches and LEDs arranged horizontally
- Lay out `FrontPanel` (`Dashboard.ts:105-121`) LEDs and DIP switches as
  horizontal rows.

### 2.6 Unified card background/title
- Give all cards one shared shell: reconcile `.card` (`styles.css:258-269`) and
  `.rlpanel`/`.disk-title` (`styles.css:637-734`) into a single card component
  with a consistent background and title treatment, used by control/front
  panels, the console card, and every device widget.

### 2.7 Verbal device status on disk widgets
- The `DiskStatus` verbal string already flows to `LiveDev.status`
  (`types.ts:28,69`, `devmodel.ts:55`) but is never rendered. Display it on the
  disk widgets (`RlWidget` and the shared `Panel`, `widgets.ts`). Depends on the
  RL status states from area 3.

---

## 3. RL02 / RL controllers

Files: `10.02_devices/2_src/rl11.{cpp,hpp}`, `.../rl0102.{cpp,hpp}`;
`10.05_web/2_src/device_status.cpp`, `.../webapi.cpp`; disk widget from 2.7.

### 3.1 Verbal status includes "spinning up" / "spinning down"
- The drive state machine already models these states
  (`RL0102_STATE_spin_up`/`_spin_down`, `rl0102.cpp`), but `disk_status()`
  (`device_status.cpp`) collapses them to `"loaded"`. Surface distinct
  "spinning up" / "spinning down" verbal strings, plumbed to the widget (2.7).

### 3.2 Properly model RL11 (Unibus) vs RLV12 (Qbus)
- Subclasses already exist: `RL11_c` (base), `RLV11_c`, `RLV12_c`
  (`rl11.hpp:33,133,138`; `rl11.cpp:203-229`), differing in address width, bus
  params/INTR level, and the BAE register for 22-bit addressing.
- Audit these against the manuals (per project rule: manual is the spec) and
  complete the differences — maintenance mode (noted unimplemented in both),
  22-bit/BAE handling, INTR level, error/status bits. **Report the specific
  gaps found before implementing.**
- Validate with the RL XXDP diagnostics per the established workflow
  (ZRLG/ZRLI/ZRLN/ZRLJ/ZRLH/ZRLK), keeping regressions clean.

---

## 4. TTY backends (all muxes / serial devices)

Files: `10.02_devices/2_src/dl11w.{cpp,hpp}`, `.../dzv11.{cpp,hpp}`,
`.../dhv11.{cpp,hpp}`, `.../serial_tcp_line.{cpp,hpp}`;
`10.05_web/3_frontend/src/lib/serial.ts`; `10.05_web/docs/api.md`.

- Introduce a polymorphic serial-transport base class implemented by:
  telnet inbound (listen) and outbound (connect) — already in
  `serial_tcp_line_c` (RFC2217); BBB serial port — already the `ttyS*`/`rs232`
  path; and a **new websocket backend** (no device-side websocket transport
  exists today).
- Wire all muxes (DL11, DZV11, DHV11) to select a backend uniformly per line at
  the current `tcp_role` / `serialport` decision point, replacing the hard-coded
  two-way branch.
- Frontend serial UI (`serial.ts`) gains the backend-type selector and websocket
  fields; document backends in `api.md`.

---

## 5. Diagnostics log

Files: `10.05_web/3_frontend/src/components/Log.ts`, `.../lib/events.ts`,
`.../store.ts`; `10.05_web/2_src/weblog.cpp`, `.../weblogging.cpp`,
`.../webevents.cpp`; `10.05_web/docs/api.md`.

### 5.1 Disk journal + paginated REST feed
- Backend writes log lines to a journal file under `/var/lib/bone` and serves
  newest-first pages via a new REST endpoint (for reload and endless scroll);
  `/ws/events` continues to stream live lines. Survives service restart.
  Document in `api.md`.

### 5.2 Newest-first display
- Render newest at the top (`Log.ts`); reconcile with the live `/ws/events`
  stream that currently pushes to the end (`events.ts:32`).

### 5.3 Reload loads the log from disk
- On page load / reload, fetch the persisted journal (5.1) instead of showing
  only lines that arrive after connect.

### 5.4 Endless scroll
- Fetch older pages from the REST feed as the user scrolls, replacing the fixed
  500-line in-memory ring (`events.ts:38`).

### 5.5 Empty-state message
- Show "no log entries matched by filter" when the filtered view is empty
  (`Log.ts` currently renders an empty box).

---

## Suggested build order

No order is committed. A sensible sequence given the dependencies:

1. **Configuration** — title, DIP-value field, and layout field are foundations
   for the dashboard.
2. **Dashboard** — grid, edit mode, cards, verbal status (consumes 1 and 3.1).
3. **RL02**, **TTY backends**, **Diagnostics** — independent; any order.
