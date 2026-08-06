# Web: configuration management — implementation plan

Manages configurations, devices, and images together on one master/detail
screen, built on the configuration model. Implements the requirements in
[`web-config-management.md`](../planning/web-config-management.md).
Built on the [configuration model](configuration-model-plan.md),
[device metadata](device-metadata-plan.md), and the Preact router
([web-navigation-plan.md](web-navigation-plan.md)).

## 1. What exists today

The backend already carries most of the data. `webconfigs.cpp` lists configs,
returns a config's full snapshot, saves the live setup under a name, applies,
deletes, and edits the `image` parameter of a drive within a stored config. The
configuration-model plan adds the current/default pointers, the modified state,
rename, default designation, and reshapes `GET /api/configs`. `webstorage.cpp`
serves the image library — `GET /api/images` (with the drives each image is
attached to), multipart upload, download, and delete with a reference check
(`webconfigs_image_referenced`).

What is missing on the backend is **general offline editing of a stored config's
device set and parameters**; only the `image` param is editable on a stored file
today. On the frontend, configurations, devices, and images live on three
separate screens.

## 2. Screen model

A **master/detail** screen at `/config`:

- **Master** — the list of configurations from `GET /api/configs`, each row
  showing its name, and marks for **current** (with its **modified** badge) and
  **default**. Selecting a row routes to `/config/<name>`
  ([web-navigation-plan.md](web-navigation-plan.md)).
- **Detail** — the selected config's device set and per-drive image assignments.
  Devices show their **friendly label** ([device-metadata-plan.md](device-metadata-plan.md))
  with the handle beside it. Selecting a device routes to
  `/config/<name>/<device>`; the active tab and filters ride query parameters.

Device management lives only inside a configuration's detail; there is no
standalone Devices page. The **image library** keeps its own page (`/storage`) for
upload/list/delete, since images exist independently of any config; **assignment**
of an image to a drive happens in the detail pane here and on the dashboard.

## 3. One editor, two Save semantics

The same device/image editor serves any configuration; the difference is what
Save does and how it is signalled, not the layout — signalled **contextually**,
not by a mode:

- **Current config** — its detail pane is headed **`Current · live`** with the
  modified badge. Edits act on the **running machine immediately** through the
  existing `/api/devices` endpoints, so the config goes *modified* (the
  configuration model computes this by comparing live against saved). **Save**
  writes the live setup back to the file (clearing modified); **Save As**, and
  **Revert** (`apply(current)`), are the config-model operations.
- **Stored (non-current) config** — its pane is headed **`Stored: <name>`**. Edits
  are **staged in the editor** and reach nothing until **Save**, which writes the
  edited document to the file. The machine is untouched — a stored config is a
  document authored offline.

The header and the Save label carry the consequence, so which one has live effect
stays obvious without a separate editing mode.

## 4. Backend: stored-config editing

Offline editing of a stored config is a **bulk write of the whole document**:

- **`PUT /api/configs/<name>`** accepts the config document
  (`{"devices": [ … ]}`) and writes it to the file atomically. This subsumes the
  configuration-model plan's "save the live setup": *Save current* sends the live
  snapshot (from `GET /api/configs?current=1`) as the body; *offline edit* sends
  the client's staged document. One endpoint, the body is always the config to
  store. Saving makes `<name>` the current configuration only when the body is the
  live setup being saved under a name; a pure stored-file edit of a non-current
  config leaves the current pointer alone. The request distinguishes them with a
  flag (`?from=live`) so the server knows whether to move the current pointer.
- Validation: the device set is checked against the known device types/params
  before the file is written, so an edited document cannot store an unknown device
  or an invalid parameter.
- Per-drive image assignment keeps its dedicated endpoint
  (`PUT /api/configs/<name>/devices/<device>/image`) for the dashboard and the
  detail pane's image picker, which both need to set one drive without rewriting
  the whole document.

`api.md` documents the body shape and the `from=live` flag.

## 5. Image assignment picker

A shared **image-assignment picker** component, used by both the detail pane and
the dashboard disk widgets ([web-dashboard.md](../planning/web-dashboard.md)),
lists the image library (`GET /api/images`) and assigns one to a drive. Against
the current config it calls the per-drive image endpoint (live effect); against a
stored config it updates the staged document. Same component, same list, so the
two screens never diverge.

## 6. Events

The screen listens to the `config` event the configuration-model plan adds
(current/default/modified) to keep the master list live, and to the existing
device `state` events for the detail pane. No new event types.

## 7. Testability

- Backend: bulk `PUT` validates and writes the document; an invalid device/param
  is rejected; `from=live` moves the current pointer while a stored edit does not.
  Rides the configuration-model host harness.
- Frontend: master/detail routing (selecting a config/device updates the URL and
  restores from it), and the picker driving the right backend for current vs.
  stored. 

## 8. Decisions

Resolved:

- **Master/detail** at `/config`, device management only inside a config's
  context; the **image library keeps its own `/storage` page**, assignment happens
  here and on the dashboard.
- **One editor**, the current-vs-stored difference signalled by a **contextual
  header + modified badge** and the Save label, no separate mode.
- Stored-config editing is a **bulk `PUT` of the whole document**; the same
  endpoint saves the live setup with `?from=live`.
- Device rows show the **friendly label** with the handle; a **shared image
  picker** serves this screen and the dashboard.

Open:

- Whether an empty selection at `/config` auto-selects the current config or shows
  an empty detail pane (shared with the navigation plan's open item).
