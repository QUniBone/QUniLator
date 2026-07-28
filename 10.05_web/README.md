# 10.05_web — QUniLator web interface

Browser UI for the `demo` application: device and parameter management,
the PDP-11 serial console, disk image handling, configuration snapshots
and live machine state — daily operation without a terminal session. The
HTTP/WebSocket server (civetweb) is embedded in the demo binary and
enabled with `demo --web [port]`.

- `2_src/` — C++ server sources, compiled into `demo` (`WEBUI=1`, default)
- `3_frontend/` — single-page frontend, served as-is; vendored xterm.js
- [`docs/plan.md`](docs/plan.md) — implementation plan (architecture, phases)
- [`docs/api.md`](docs/api.md) — REST/WebSocket reference
- [`docs/mockup.html`](docs/mockup.html) — clickable UI mockup, the design
  reference for the frontend
- `tools/` — host-side fixture server for frontend work without a BeagleBone

## Running

```sh
sudo QUNILATOR_DIR=$HOME/QUniBone ./10.03_app_demo/4_deploy_q/demo --web --addresswidth 22
```

`--web` takes an optional port (default 80). The application brings up the
emulated device set at startup and enters the usual menu — the CLI and the
web interface operate the same devices side by side; web actions echo on
the terminal as `web: ...` lines. The hardware test menus are unavailable
while the web interface owns the devices.

Disk images live in `$QUNILATOR_DIR/images/`, configuration snapshots in
`$QUNILATOR_DIR/configs/`.

## Access control

Set `WEBUI_PASSWORD` to require HTTP basic auth (any user name, this
password). The natural place is `qunibone-platform.env`:

```sh
export WEBUI_PASSWORD=secret
```

Unset, access is open — appropriate only on a trusted bench LAN.

## Autostart

A systemd unit runs the web interface headless at boot:

```ini
# /etc/systemd/system/qunibone-web.service
[Unit]
Description=QUniBone web interface
After=network.target

[Service]
Environment=QUNILATOR_DIR=/home/hans/QUniBone
EnvironmentFile=-/home/hans/QUniBone/qunibone-platform.env
WorkingDirectory=/home/hans/QUniBone
ExecStart=/home/hans/QUniBone/10.03_app_demo/4_deploy_q/demo --web --addresswidth 22
StandardInput=null
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now qunibone-web
```

With stdin from `/dev/null` the menu loop idles and all operation happens
through the browser. For interactive use, run in `tmux` instead so the CLI
stays reachable.

## Development

`crossbuild.sh` (repository root) cross-compiles the binary in a Docker
container in under a minute and deploys it to the device with `-d` — see
the script header. Frontend work runs against the fixture server on the
development machine (`tools/host_test.cpp`, build instructions in its
header); it serves `3_frontend` with canned API responses.
