# QBone — next development round

Requirements for the next round of QBone work, refined here before any code is
written. Each area of work has its own document. Every document captures what
the feature must do, where the code stands today, and an **Open questions**
section that drives the next round of refinement.

These are requirements, not designs. They say what the software must do and
why. Implementation plans (the `*-plan.md` files in [`docs/plans/`](../plans),
e.g. [`vcb01-plan.md`](../plans/vcb01-plan.md)) come after an area's requirements
settle. The shipped web design lives in [`web-interface-plan.md`](../plans/web-interface-plan.md).

## How we work here

1. Ideas land in the relevant area document as draft requirements.
2. Open questions collect what still needs a decision.
3. Once an area's questions are answered, it is marked ready and an
   implementation plan follows.

Status legend, shown at the top of each document:

- **Gathering** — requirements still being collected; open questions unresolved.
- **Ready** — requirements settled; ready for an implementation plan.
- **In progress** — implementation underway.

## Areas of work

| Area | Document | Status |
|---|---|---|
| Configuration model | [configuration-model.md](configuration-model.md) | Ready — [plan](../plans/configuration-model-plan.md) |
| Web: configuration management | [web-config-management.md](web-config-management.md) | Ready — [plan](../plans/web-config-management-plan.md) |
| Web: dashboard | [web-dashboard.md](web-dashboard.md) | Ready — [plan](../plans/web-dashboard-plan.md) |
| Web: navigation and URL state | [web-navigation.md](web-navigation.md) | Ready — [plan](../plans/web-navigation-plan.md) |
| Console | [console.md](console.md) | Ready — [plan](../plans/console-plan.md) |
| Device metadata (friendly names) | [device-metadata.md](device-metadata.md) | Ready — [plan](../plans/device-metadata-plan.md) |
| Serial ports over TCP | [serial-ports.md](serial-ports.md) | Ready — [plan](../plans/serial-ports-plan.md) |
| VCB01 support | [vcb01.md](vcb01.md) | Ready — [plan](../plans/vcb01-plan.md) |
| Logging control | [logging.md](logging.md) | Ready — [plan](../plans/logging-plan.md) |
| RSX-11M+ over DELQA | [rsx-delqa.md](rsx-delqa.md) | Ready — [plan](../plans/rsx-delqa-plan.md) |
| MCP server | [mcp-server.md](mcp-server.md) | Ready — [plan](../plans/mcp-server-plan.md) |
| Configuration repository | [configuration-repository.md](configuration-repository.md) | Gathering |
| Device implementation standard | [device-implementation-standard.md](device-implementation-standard.md) | Standing |

## Cross-cutting decisions

These apply across the areas above and are fixed for this round:

- **Frontend gains a build step.** The web UI moves to **Vite + Preact +
  TypeScript**, bundled on the dev machine; the static output is still served
  as-is by civetweb, so nothing new runs on the board. All the web areas
  (dashboard, config management, navigation, console) build on this.
- **The REST API may change shape.** The web UI, MCP server, and board version
  together — one deployment, no external clients — so endpoints are reshaped
  freely as the configuration model needs, and `api.md` is updated to match.
- **New API work carries automated tests.** New endpoints, the configuration
  model above all, ship with integration tests runnable in CI (Forgejo Actions).
  Device emulation is validated by XXDP per the
  [device implementation standard](device-implementation-standard.md).

## Cross-cutting dependencies

The configuration model underpins the configuration-management UI and the
default-configuration and save/rename/delete behaviour. Friendly device names
surface in both the dashboard and the configuration-management screens. The
dashboard disk widgets depend on the same image list the configuration screens
manage.

**Implementation order:** the **configuration model is planned and built first**,
as the keystone the config UI, dashboard drive-swap, and MCP config tools sit on.

## Implementation agents

The work splits along skill and toolchain lines into four specialist agents
(defined in [`.claude/agents/`](../../.claude/agents/)). They are skill
specialists that hand off across the `api.md` contract, not owners of whole
plans — several plans deliberately span more than one.

| Agent | Owns | Plans |
|---|---|---|
| `frontend-web` | Vite/Preact/TS SPA: routing, dashboard, console UI, config screen, widgets | web-navigation, console (UI), web-config-management, web-dashboard (UI) |
| `service-cpp` | civetweb REST/WS API, config model, settings, events, console channel, logging | configuration-model, logging, device-metadata, console (server), web-config-management (API), web-dashboard (power flag, `status`) |
| `device-emulation` | `qunibusdevice_c` devices: registers/DMA, PRU/bus, XXDP | serial-ports (register frontends), vcb01 (device) |
| `pdp11-guest` | RSX-11M+ SYSGEN/NETGEN, 2.11BSD drivers, MACRO-11, boot/test | rsx-delqa, vcb01 (2.11BSD driver) |

Shared contracts to keep aligned: the REST/WS shapes and the `config`/`state`
events, the device `label` and verbal `status` fields (service-cpp owns them,
frontend-web and the MCP server read them), and the serial-port TCP/RFC2217
backend (service-cpp) behind the emulated mux registers (device-emulation).
