# 10.05_web — QUniLator web interface

The whole of operating a machine, in a browser: the dashboard and its widgets,
disk and tape images, saved configurations, machine control, the serial console,
the debug workbench, the log, and the system page that updates the software in
place. It is what an operator uses; the interactive menu (`<name>-cli`) covers
the hardware-level work below it.

The HTTP/WebSocket server is civetweb, embedded in the emulator binary. Both
binaries carry it — the service `/usr/bin/<name>` serves it as its whole job,
and `<name>-cli` can serve it too with `--web [port]`.

- `2_src/` — the C++ server: REST endpoints, the event and console WebSockets,
  the configuration model, settings, logging control and self-update
- `3_frontend/` — the single-page frontend, Vite + Preact + TypeScript
- [`docs/api.md`](docs/api.md) — the REST/WebSocket reference. It is a contract:
  update it with every shape change.
- `tools/` — host-side tests and a fixture server for frontend work without a
  BeagleBone

The implementation plan this was built to is
[`docs/plans/web-interface-plan.md`](../docs/plans/web-interface-plan.md),
kept as a record rather than as current documentation.

## Running it

On a card flashed from the release image there is nothing to start: the package
installs `<name>.service` and it serves port 80 from boot. `systemctl status
qbone` (or `unibone`) says whether it is up.

State lives in `/var/lib/qunilator` — `images/` for disk and tape images,
`configs/` for saved configurations, `settings.json` for board settings the
service applies at startup. The frontend is served from disk, out of
`/usr/share/qunilator/frontend`.

## Access control

The first-run dialog creates one identity that is both the operator's account on
the BeagleBone and the web login, so the same name and password reach the web
interface, the file shares and an ssh session. Every request then takes HTTP
basic auth, and **the name is part of the credential**: the right password under
the wrong name answers `401`. `PUT /api/auth` changes it.

A board that carries no credential answers everything, which suits a bench and
nothing else.

## Building the frontend

```sh
cd 3_frontend
npm install
npm run build      # tsc --noEmit, then vite build
npm run dev        # Vite dev server, proxying /api to a board
```

`crossbuild.sh -d` from the repository root deploys the binary;
`QUNILATOR_DEPLOY_FRONTEND=1 ./crossbuild.sh -d` deploys the built bundle with
it. There is no frontend-only deploy — the swap happens inside the appliance
deploy, after the binary is replaced and the service bounced, so a UI change
costs the running machine.

## Working without hardware

`tools/host_test.cpp` runs `webserver_c` on the development host against stubbed
logger and hardware interfaces, serving `3_frontend` and the `/api/` endpoints.
Build and run instructions are in its header. The other files in `tools/` are
host tests for the configuration model, the console channel, authentication,
recordings, the serial TCP line and SimH tape handling; `run_config_test.sh`
drives the configuration one.

Every change to the web interface is verified in a real browser against a board
before it is called done — `tsc --noEmit` and `vite build` say nothing about
layout, focus, drag behaviour or whether a widget draws at all.
