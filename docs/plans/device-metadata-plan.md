# Device metadata (friendly names) — implementation plan

Gives every device a plain-language **friendly name** shown in the web UI beside
its terse handle, drawn from a static per-type table. Implements the requirements
in [`device-metadata.md`](../planning/device-metadata.md).

## 1. What exists today

`GET /api/devices` (`webapi.cpp`, `devices_list`) emits one object per device
with `name` (the handle, `uda0`), `type` (the device's `type_name`, `RA81`),
`parent`, and the parameter list. The type registry is a controller/drive tree:
controllers (`UDA50`, `RLV12`, `RKV11`, `RF11`, `RXV11`, `RXV12`) each parent a
set of drive instances (`RA81`, `RL02`, `RK05`, `RS11`, `RX01`, `RX02`); the
standalone devices are `slu_c` (DL11), `ltc_c` (KW11), `DELQA`, `VCB01`, and the
internal `blinkenbone_c`, `demo_io_c`, and `RX0102uCPU`.

Each drive carries a `unit` parameter holding its instance number. The two serial
lines share `type_name` `slu_c` and are told apart by `base_addr` (`DL11` at
777560, `DL11b` at 776500).

The handles alone require hardware knowledge to read; nothing today presents a
device's role in words.

## 2. The friendly name

- A **static table compiled into the board version**, keyed by `type_name`. Each
  entry gives a role description and the DEC code, rendered as
  **`<role> (<code>)`** — for example `RA81` → `MSCP disk (RA81)`. The `type`
  field keeps carrying the raw code, so the label never removes information.
- **Instanced devices** (the child drives) append their `unit` number to the
  role: `uda0` → `MSCP disk 0 (RA81)`. A controller or standalone device has no
  unit and shows the bare role: `uda` → `MSCP disk controller (UDA50)`.
- **Same-type instances disambiguated by address.** The two `slu_c` lines both
  resolve to the `DL11` entry; their label carries the CSR address so they read
  distinctly — `DL11` → `Serial line unit @777560 (DL11)`, `DL11b` →
  `Serial line unit @776500 (DL11)`.
- **Internal devices are named too**, so nothing shows a bare handle.

### The table

| `type_name` | Role | Code | Instanced |
|---|---|---|---|
| `blinkenbone_c` | Front panel | — | no |
| `demo_io_c` | Demo I/O | — | no |
| `RF11` | DECdisk controller | RF11 | no |
| `RS11` | Fixed-head disk | RS11 | yes |
| `RLV12` | RL disk controller | RLV12 | no |
| `RL02` | RL02 cartridge disk | RL02 | yes |
| `RKV11` | RK disk controller | RKV11 | no |
| `RK05` | RK05 cartridge disk | RK05 | yes |
| `UDA50` | MSCP disk controller | UDA50 | no |
| `RA81` | MSCP disk | RA81 | yes |
| `slu_c` | Serial line unit | DL11 | by address |
| `ltc_c` | Line-time clock | KW11 | no |
| `RXV11` | RX01 floppy controller | RXV11 | no |
| `RXV12` | RX02 floppy controller | RXV12 | no |
| `RX0102uCPU` | Floppy microcontroller | — | no |
| `RX01` | RX01 floppy | RX01 | yes |
| `RX02` | RX02 floppy | RX02 | yes |
| `DELQA` | Ethernet controller | DELQA | yes |
| `VCB01` | Graphics display | VCB01 | no |

`DELQA` is marked instanced so a second interface would number cleanly; with one
present the unit is omitted. A `type_name` absent from the table falls back to the
raw handle, so an unrecognised device still renders.

## 3. API change

`GET /api/devices` gains a **`label`** field on each device, computed from the
table at response time:

```json
{ "name": "uda0", "type": "RA81", "parent": "uda",
  "label": "MSCP disk 0 (RA81)", "params": [ … ] }
```

The label is derived, held nowhere on the device, and is read-only — there is no
setter and no persistence, matching "not user-editable". `api.md` documents the
field.

The label builder lives in `webapi.cpp` beside `devices_list` (or a small
`device_label.cpp/.hpp` if it grows), taking `type_name`, the `unit` parameter,
and `base_addr` and returning the rendered string. Both the web UI and the MCP
server read it from the API, so the table has one home.

## 4. Consumers

- **Web UI** shows the label as the primary device name with the handle beside or
  under it, across the device lists in the configuration-management screen and the
  dashboard widgets ([web-config-management.md](../planning/web-config-management.md),
  [web-dashboard.md](../planning/web-dashboard.md)).
- **MCP server** surfaces the label in its device-status tool
  ([mcp-server.md](../planning/mcp-server.md)), reading the same API field
  rather than carrying its own copy.

## 5. Testability

A host-build unit test over the label builder: feed it each `(type_name, unit,
base_addr)` triple from the table and assert the rendered string — controller vs.
instanced drive, the two SLUs by address, and the raw-handle fallback for an
unknown type. This rides the host test harness the configuration-model plan
introduces.

## 6. Decisions

Resolved:

- The table is a **static per-type map compiled into the board version**; the API
  exposes it as a computed **`label`** field. One source of truth for the web UI
  and MCP.
- Label form is **`<role> (<code>)`**, description first with the DEC code in
  parentheses; the raw code stays in `type`.
- **Instanced drives append their `unit`**; the two `slu_c` lines disambiguate by
  CSR address.
- **Internal devices are named** (Front panel, Demo I/O, Floppy microcontroller)
  rather than shown as bare handles.

Open:

- The exact prose for a few roles (`RF11` "DECdisk controller", the floppy
  microcontroller) can be tightened against the DEC manuals when the table is
  written.
