# Web: configuration management

**Status:** Ready — implementation plan drafted in
[`web-config-management-plan.md`](../plans/web-config-management-plan.md).

Configurations, devices, and images are managed together instead of on three
separate screens.

## Current state

The web UI presents configurations, devices, and disk images on separate pages.
Managing a machine setup means moving between them: pick a configuration on one
screen, adjust devices on another, attach images on a third. The relationships
between the three are not visible in one place.

## Requirements

- Configurations, devices, and images are manageable together, so a machine
  setup can be understood and changed without moving between screens.
- **Master/detail layout.** A list of configurations on one side; selecting one
  shows its devices and image assignments on the other. The **current** config
  and the **default** config are marked in the list (and the current shows its
  modified state).
- Builds on the [configuration model](configuration-model.md): the current
  configuration, Save / Save As / Revert, rename, delete, and the designated
  default are operated from here.

- **Any configuration is editable in place.** Selecting a non-current config lets
  its devices and image assignments be edited and saved **without applying it** to
  the live machine — configurations are documents that can be authored offline.
  Editing the current config's saved form is distinct from the live machine's
  dirty state ([configuration-model.md](configuration-model.md)).

## Decisions

- **Master/detail layout** (configs list ↔ selected config's devices + images).
- **Edit any config in place**, saving without applying.
- **One editor for all configs.** The same device/image editor serves any
  configuration; editing the current config tracks the live dirty state, editing
  a stored config just writes the file. The difference is in what Save does, not
  the UI.
- **Device management lives only in a configuration's context** — there is no
  standalone Devices page. **Images keep a standalone library page** (upload,
  list, delete) since images exist independently of any config; assignment
  happens in the config screen and the dashboard.

## Open questions

- With one editor for all configs, editing the current config touches the live
  machine's dirty state while editing a stored config does not — how is that
  difference signalled so the consequence stays obvious without a separate mode?
- Images exist independently of any configuration (upload, list, delete). Where
  does image *library* management live versus image *assignment* to a drive?
- How are device friendly names ([device-metadata.md](device-metadata.md)) shown
  and used for selection here?
- Which actions belong here versus the [dashboard](web-dashboard.md) (e.g.
  selecting an image for a removable drive is wanted in both places)?
