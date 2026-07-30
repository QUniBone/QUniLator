# Self-update — implementation plan

The web interface tells the operator when a newer package is published, shows
what changed since the version the board runs, and installs it on one click.
The install survives the operator watching it happen: the service that serves
the page is the thing being replaced, so the page rides the restart out and
comes back on the new version by itself. A board left unattended mid-update
comes back either fully updated or fully on the version it had.

The emulator package is the subject: `qbone` on a QBUS cape, `unibone` on a
UNIBUS one. The interface also reports the rest of the board's packages and can
upgrade them (§10), with a much narrower promise about what that is worth.

## 1. What exists today

`packaging/build-deb.sh` builds the package and takes its version from the top
stanza of `packaging/debian/changelog`, which is also the release-note source in
`.github/workflows/release-deb.yml`. That workflow builds both boards' packages,
attaches them to the tag's GitHub Release, and pushes both over an ssh forced
command to the apt repository host, which drops them into `pool/main` and
rebuilds the signed index. `packaging/build-image.sh` writes
`/etc/apt/sources.list.d/<name>.list` with a `signed-by` keyring when
`APT_REPO_URL` and `APT_REPO_KEY_URL` are given, so a flashed board can
`apt update` / `apt upgrade` already. A package is ~1.7 MB.

The unit runs the emulator as root with no `User=`, so a helper it starts has
the privileges an install needs. `Restart=no`: a binary that dies on startup
leaves the board with no web interface until someone logs in.

The service knows nothing about its own version. `GET /api/state` reports
`{platform, api_version, images_dir}`, and the only version string in the tree
is `VERSION "v1.5.0"` in `10.03_app_demo/2_src/application.hpp:47`, seven
releases stale — the startup banner prints it, nothing checks it.

On the frontend, `lib/events.ts` already reconnects `/ws/events` every 2 s and
drives `store.connected`, which the topbar shows as a link LED. The bundle is
one file with no dynamic imports, and civetweb serves the docroot with
`static_file_max_age 0`, so a reload after the restart fetches the new bundle
rather than a cached one. The store carries `configModified`, and the dashboard
derives RUN from the state frames — both are needed to warn about what an update
interrupts.

`DISTRIBUTION.md` still says only the QBUS package is published. The workflow
pushes both; that note wants correcting when this lands.

## 2. Version and package identity

Everything below compares versions and names a package, so the board has to be
able to state both, and the two brands must resolve without a second code path.

**The version.** `packaging/debian/changelog` stays authoritative, and three
consumers derive from it:

- **The binary.** The makefiles pass
  `-DQUNILATOR_VERSION="$(shell packaging/version.sh)"` and
  `application.hpp` uses it in place of the literal. `packaging/version.sh` is
  the one-line `sed` `build-deb.sh` already has, factored out so the package and
  the binary cannot disagree.
- **The bundle.** `vite.config.ts` reads the same file and defines
  `__QUNILATOR_VERSION__`, so the page knows which version it was built from. A
  developer's plain `npm run build` gets it too.
- **The package.** `build-deb.sh` calls `packaging/version.sh`.

**The brand.** The emulator knows its own from the compile-time bus switch, the
same one `platform_name` uses: QBUS is `qbone`, UNIBUS `unibone`. That name is
the package, the unit (`<name>.service`), the binary (`/usr/bin/<name>`) and the
apt source file (`/etc/apt/sources.list.d/<name>.list`).

The updater cannot use that switch: `build-deb.sh` installs the `qunilator-*`
scripts and units into both packages **unrebranded**, which is what keeps them
one implementation. So the updater resolves its own brand at runtime from the
package that owns it:

    pkg=$(dpkg -S /usr/sbin/qunilator-update | cut -d: -f1)

One shipped script, self-identifying, no brand baked in and no duplicate for the
other bus. Every apt, `systemctl` and path expression below derives from `$pkg`.

`GET /api/version` then answers

```json
{"package": "qbone", "version": "1.13.0-1", "built": "2026-07-30T09:12:00Z",
 "api_version": 0}
```

The frontend compares `__QUNILATOR_VERSION__` with what this reports on every
reconnect and reloads when they differ. That closes the general case as well: a
hand-run `apt upgrade` over ssh also gets every open page onto the matching
bundle.

The service additionally writes `/run/qunilator/version` at startup, holding the
same version string. It is what proves to the updater that the instance now
running is the new one (§7 step 8) without an authenticated request and without
an unauthenticated endpoint.

CI gains one check per matrix platform: the compiled-in version equals the
changelog version.

## 3. The update source: the apt repository, baked into every image

The check is `apt-get update` plus `apt-cache policy`, the install is `apt-get
install`, and apt verifies the signed index and the package hash — so there is
no unsigned install path, nothing new to host and nothing new to sign. It is
also the mechanism an operator already uses by hand over ssh.

`release-image.yml` always passes `APT_REPO_URL` and `APT_REPO_KEY_URL`, so
every published image carries the keyring and the source entry and self-update
works on a freshly flashed card with no setup. Both brands' packages live in the
same repository, distribution `trixie`, component `main`; the `Conflicts` /
`Replaces` pair between them means a board is only ever offered the brand it has,
and the per-brand source file and keyring name keep the two boards' apt
configuration independent even though they point at one repository. This commits
the repository host to being reachable for every board shipped.

A board installed by hand with `dpkg -i` has no source entry: it reports
`source: none`, and the interface says so once rather than nagging.

Considered and set aside: the GitHub Release assets, which are unsigned and
would need their own downloader and version discovery; and image-level A/B
updates (RAUC, Mender, SWUpdate), which are the right long-term shape for the
kernel and base OS and a project of their own — a package update does not need a
partition scheme to be safe, since dpkg either completes or leaves the old
version installed, and §7 adds the rollback that covers what dpkg cannot see.

## 4. The check: a systemd timer, published by the service

`qunilator-update-check.timer` fires 5 min after boot and daily with a
randomised delay, runs `qunilator-update --check`, and writes the result to a
status file. The emulator's own threads never wait on the network, and an
operator can run the same command by hand.

The service publishes what the timer wrote: it stats the status file once a
second in the event-poll thread it already runs, and broadcasts an `update`
frame when the mtime moves. `POST /api/update/check` starts
`qunilator-update-check.service` with `systemctl start --no-block` and answers
202; the result arrives as an event.

Considered and set aside: a thread inside the service, which would put a network
fetch that can hang for minutes inside the process that drives the bus; and a
check from the browser, which only runs while a page is open.

## 5. The changelog: extracted from the candidate package

`apt-get download` the candidate into the staging directory, `dpkg-deb
--fsys-tarfile` out `usr/share/doc/$pkg/changelog.Debian.gz`, and cut the
stanzas newer than the installed version with `dpkg --compare-versions`. The
package already ships the complete changelog, so this needs no new publishing
and no new format, the text shown is the text that shipped byte for byte, and
the download doubles as the staging step the install wants anyway — at 1.7 MB it
costs nothing to fetch before the operator has decided.

Considered and set aside: apt's `Changelogs` endpoint, which needs a layout the
receive script does not build; and a JSON manifest published beside the
repository, which is a second artifact to keep in step with the changelog.

## 6. The install runs in its own systemd unit

The install stops and restarts the emulator's unit, so it cannot be a child of
it: systemd kills the unit's cgroup on stop, and a `dpkg` interrupted there is
the one failure mode most worth engineering against.

`qunilator-update.service` is `Type=oneshot`,
`ExecStart=/usr/sbin/qunilator-update --install-requested`. The service writes
the requested version to a request file and does `systemctl start --no-block
qunilator-update.service`. The updater then lives in its own cgroup, untouched
by anything that happens to the emulator, and no string from an HTTP request
ever reaches a command line — the requested version is validated against the
candidate apt reports, refused if it differs, and written to the request file.

Considered and set aside: `systemd-run`, which builds a command line out of a
client-supplied version; and `fork()` + `setsid()` from the service, which
escapes the process tree but not the cgroup, so `systemctl stop` kills it
mid-`dpkg`.

## 7. The updater

One shipped script, `/usr/sbin/qunilator-update`, is the whole board-side
mechanism. It is the only thing that runs apt, and it owns the status file.

    --check              refresh the index, report installed vs candidate,
                         and what else on the board has updates
    --changelog          stage the candidate, emit the stanzas since installed
    --install-requested  install the version named in the request file
    --os-upgrade         upgrade the rest of the board's packages (§10)
    --rollback           reinstall the cached previous package
    --status             print the status file

State lives in `/var/lib/qunilator/updates`, mode 0700 root:root — it sits
inside the state directory the file shares are chrooted to, so the mode matters:

    status.json     check result and the last or running install
    request.json    the version the interface asked for
    cache/          the package the board runs, kept for rollback
    staging/        the downloaded candidate

`--check` runs `apt-get update` against `$pkg`'s own source alone
(`-o Dir::Etc::sourcelist=/etc/apt/sources.list.d/$pkg.list -o
Dir::Etc::sourceparts=-`), so a stale Debian mirror cannot make the check slow
or red, then reads `apt-cache policy $pkg` and writes `status.json`. It also
runs `dpkg --audit`; a package system left needing repair is reported, so the
interface can say so before an install is attempted. A candidate older than the
installed version reports `state: "ahead"` — a development board carrying a
hand-built package is offered nothing and told why.

`--install-requested`, each step written to `status.json` as it starts:

1. Take an exclusive `flock` on `status.json`. A second request is refused.
2. `dpkg --configure -a` if `dpkg --audit` reports an interrupted install.
3. Check free space on `/` against the package's `Installed-Size` with a margin.
   A full root filesystem is what turns a routine `dpkg` into a repair job.
4. `apt-get update` across all sources, tolerating failures from any source
   other than `$pkg`'s, so a new dependency can still resolve.
5. `apt-get download $pkg=<version>` into `staging/`. apt checks the hash
   against the signed index; nothing is installed until this succeeds.
6. Populate `cache/` with the package the board currently runs — the copy left
   by the previous update, or `apt-get download` of the installed version. If
   neither is available, continue with `rollback: false` recorded.
7. `apt-get install -y --allow-change-held-packages -o
   Dpkg::Options::=--force-confold -o DPkg::Lock::Timeout=300
   staging/$pkg.deb`. Installing the local file lets apt resolve dependencies
   from the repository; `--force-confold` keeps the operator's edited
   `network.conf`. The package's own `postinst` restarts `$pkg.service`, which
   was active.
8. Health check, up to 60 s: `systemctl is-active $pkg.service`; a loopback
   `GET http://127.0.0.1/api/version`, where 200 or 401 both prove the server
   answers; and `/run/qunilator/version` holding the new version, which proves
   the instance answering is the new one on a password-protected board too. Then
   require the unit to still be active 10 s later, so a service that comes up
   and dies is caught rather than declared well.
9. Failure at 7 or 8 rolls back: `apt-get install -y --allow-downgrades
   cache/$pkg.deb`, health-check again, and record the outcome with the last 50
   journal lines of the failed unit, so the interface can show why.
10. Write the terminal state (`done`, `failed`, `rolled-back`) with both
    versions and a timestamp. The new service instance reads `status.json` at
    startup, so the reconnecting page is told how it went by the process that
    replaced the one it was talking to.

`--rollback` is step 9 on demand, for a board that came up on a new version
which runs but misbehaves. It stays a command-line action: the automatic
rollback covers the failure the interface can detect, and a deliberate step back
is rare enough to be worth an ssh session — which also keeps the interface from
having to explain the cases where `cache/` is empty.

## 8. What makes it fail-safe

| what fails | what happens | what the operator sees |
|---|---|---|
| No repository configured, or unreachable | check reports it, nothing else runs | "no update source configured" / "the repository could not be reached" |
| Download interrupted or hash mismatch | apt aborts before dpkg starts; the old version stays installed and running | the install fails with the apt message, the machine keeps running |
| Root filesystem full | refused at step 3, before dpkg | "not enough space", with the figure |
| dpkg interrupted by power loss | the next `--check` reports the package system needs repair; the next install repairs it first | a banner saying repair is needed |
| New service will not start | the health check fails, the cached package is reinstalled, the service comes back on the old version | "the update was rolled back", with the journal tail |
| Web service killed mid-update | the updater is a separate unit in its own cgroup and runs to completion | the page reconnects to whichever version the updater settled on |
| Two operators press Install | the second is refused by the `flock` | "an update is already running" |
| A version the repository does not offer is requested | refused before the request file is written | "that version is not available" |

The state directory is the operator's, not the package's: `postinst` seeds
`configs/default.json` only when it is absent, and images, configurations and
`settings.json` are never rewritten by an install. An update cannot lose a
machine's configuration or its disks.

## 9. What an update costs, and how the dialog says so

Replacing the service stops the emulated machine. The guest is not shut down —
the SIGTERM handler shuts the device set down in order, so the images are
flushed, but a running Unix or RSX has its buffer cache cut. And the board comes
back up in its startup configuration, which is not necessarily the one it was
running.

The install dialog therefore states, above the button:

- the version it is moving to, and the changelog since the installed one;
- that the emulated machine stops, with the live run state shown — while the
  machine is running, the dialog carries a **halt the machine** button that
  posts `halt`, and Install needs a confirmation checkbox;
- that the board restarts into its startup configuration, and — when
  `configModified` is set — that the live configuration has unsaved changes,
  with a link to save them first;
- that the interface reconnects on its own, in about half a minute.

## 10. Operating-system updates

`--check` also reports what else the board could upgrade: `apt list
--upgradable` after a full `apt-get update`, minus `$pkg` itself, written to
`status.json` as `os: {count, packages: [{name, from, to}], held_back: […],
reboot_required: bool}`. `reboot_required` is `/var/run/reboot-required`.

`--os-upgrade` installs them, in the same isolated unit and under the same
`flock`, so an OS upgrade and an emulator update can never overlap:

1. `dpkg --audit` repair and a free-space check, as in §7.
2. `apt-mark hold $pkg` for the duration, released at the end. The emulator is
   upgraded only by the deliberate path in §7, so the one action that stops the
   operator's machine stays the one action that warns about it.
3. `DEBIAN_FRONTEND=noninteractive apt-get -y -o
   Dpkg::Options::=--force-confold -o DPkg::Lock::Timeout=300 upgrade`.
   `upgrade`, so nothing is removed and no new package is pulled in; whatever
   that holds back is reported rather than forced.
4. Record the outcome, the packages upgraded, and whether a reboot is now
   required.

**This path has no rollback, and the plan should not pretend otherwise.** apt
cannot undo an upgrade, so a base-OS upgrade that breaks the board is an ssh
session or a reflash. Three things keep the blast radius small: the kernel stays
held by `apt-mark hold` from the image build, so an OS upgrade cannot move the
one component the whole cape port depends on; `upgrade` rather than
`full-upgrade` means no package is removed to satisfy another; and the emulator
package is held out of it, so a running machine is not stopped by an OS upgrade
at all. The dialog says plainly that this is not the reversible path the
emulator update is, and that a reboot — which does stop the machine — is the
operator's own call afterwards. Nothing here ever reboots the board.

On the System page this is its own row, separate from the emulator update:
`N other packages have updates` with the list, an **Upgrade the operating
system** button behind the warning above, and the reboot-required notice when it
applies. The sidebar badge announces the emulator package only. The frontend
follows an OS upgrade through the same `update` event frames; the socket stays up
throughout, since the emulator is not being replaced.

## 11. API

| | |
|---|---|
| `GET /api/version` | `{package, version, built, api_version}` (§2) |
| `GET /api/update` | the status file, plus `source_configured` and `changelog` |
| `POST /api/update/check` | starts the check unit, 202 |
| `GET /api/update/changelog` | the staged candidate's stanzas since installed |
| `POST /api/update/install` | `{"version": "1.13.0-1"}`; validated against the candidate, writes the request file, starts the unit, 202 |
| `POST /api/update/os` | starts the OS upgrade unit, 202 (§10) |
| `POST /api/update/dismiss` | `{"version": …}` — stop announcing this one |

`GET /api/update` answers the shape the status file holds:

```json
{"state": "idle", "package": "qbone", "source_configured": true,
 "checked_at": "2026-07-30T09:12:00Z", "installed": "1.12.0-1",
 "candidate": "1.13.0-1", "rollback": true, "needs_repair": false,
 "dismissed": "", "os": {"count": 4, "packages": [], "held_back": [],
 "reboot_required": false}, "last": {"state": "done", "from": "1.11.0-1",
 "to": "1.12.0-1", "at": "2026-07-28T06:40:11Z"}, "error": "", "journal": []}
```

`state` is `idle`, `checking`, `ahead`, `downloading`, `installing`,
`verifying`, `os-upgrading`, `done`, `failed` or `rolled-back`.

On `/ws/events`, `{"t":"update", …}` carries the same object, broadcast when the
status file changes and as a snapshot on every new connection — so a second tab,
and a tab opened during an install, both know what is going on. `phase` messages
during the run give the overlay something to show.

Dismissal is stored in `settings.json` (`update.dismissed_version`) so it
belongs to the board rather than to one browser, and a later version announces
itself again.

## 12. Frontend

**Announcement.** When the candidate is newer than the installed version and is
not the dismissed one, a badge appears in the sidebar under the wordmark and a
pill in the topbar: `Update 1.13.0`. Both route to the update page.

**Where it lives.** A new nav entry, **System**, holding the installed version
and build date, the board's package name, the check result with a **Check now**
button, the changelog since the installed version, **Install** and **Dismiss**,
the OS-update row of §10, and the outcome of the last update. The Machine page
keeps its stated scope of machine-wide hardware settings.

**The install choreography**, the part that has to survive its own server going
away:

1. Install posts, the page writes `{from, to, started}` into `sessionStorage`
   and raises a modal overlay. The flag means a manual reload during the window
   still shows an update in progress rather than a connection error.
2. `update` frames drive the overlay until the socket drops, which is expected
   and is not shown as a fault.
3. From then on the overlay polls `GET /api/version` every second for up to
   120 s, treating a connection refusal as "not back yet".
   - Version equals the target: clear the flag and `location.reload()`, which
     fetches the new bundle — one file, revalidated, no stale chunks.
   - Version still the old one: read `GET /api/update`. `failed` or
     `rolled-back` shows the error and the journal tail; anything else keeps
     waiting, since `postinst` may not have restarted the service yet.
   - Timeout: the overlay says the board has not come back and names what to
     look at — `systemctl status <pkg>`, `journalctl -u qunilator-update` — and
     leaves the page open rather than pretending to be connected.
4. Any page reaching a server whose version differs from
   `__QUNILATOR_VERSION__` reloads, whether or not it started the update. This
   is what carries a second tab, and an `apt upgrade` run over ssh, onto the
   matching bundle.

## 13. What the package gains

    /usr/sbin/qunilator-update                          the updater (§7)
    /lib/systemd/system/qunilator-update.service        oneshot, install
    /lib/systemd/system/qunilator-update-os.service     oneshot, OS upgrade
    /lib/systemd/system/qunilator-update-check.service  oneshot, check
    /lib/systemd/system/qunilator-update-check.timer    boot+5min, daily
    /etc/apt/apt.conf.d/51qunilator-unattended          unattended blacklist
    /var/lib/qunilator/updates/                         0700 root:root

All of it is bus-agnostic and installed unrebranded into both packages, as the
other `qunilator-*` tools are; §2 is how one script serves both brands.

`postinst` enables the check timer, on upgrade as well as on first install, so a
board updated once starts checking. apt, dpkg and systemd are already there, so
`Depends` gains only what the health check needs to speak HTTP to the loopback:
`curl`. The appliance image does not install it today — `build-image.sh` uses
curl in the build container, not in the chroot — so the dependency has to be
declared rather than assumed.

**Unattended upgrades must not touch the emulator package.** An unattended
upgrade would stop the operator's running machine with no warning at all, which
is what §9 exists to prevent. The `apt.conf.d` drop-in blacklists both brands;
the interface's own install passes `--allow-change-held-packages`, so an
operator who additionally holds the package by hand is not blocked from a
deliberate update.

## 14. Testing

On the board, in order — on `unibone` first, which exercises the UNIBUS brand
and is the board free to disturb, then confirmed on `qbone`:

- install the previous release by hand, then update through the interface with
  the page open; confirm the machine's state, the reconnect and the reload.
- the same with two tabs open, one of which did not start the update.
- a deliberately broken package — one whose binary exits immediately — to prove
  the health check fails it and the rollback restores the running version.
- the repository made unreachable, to see the check and the install both refuse
  cleanly.
- an interrupted `dpkg` (SIGKILL mid-install) followed by a check, to see the
  repair path reported and then taken.
- an install while the machine runs a 2.11BSD multi-user system, to confirm the
  warning is accurate about what happens to it.
- an OS upgrade with the emulated machine running, to confirm it keeps running,
  the kernel is held back, and the reboot notice appears when it should.
- `dpkg -S` brand resolution on both boards, and a check on each that only its
  own brand is ever offered.

In CI, for both matrix platforms: the compiled-in version equals the changelog
version, and the new units and the updater script pass `systemd-analyze verify`
and `sh -n`, beside the maintainer-script check `build.yml` already does.

## 15. Work in order

1. **Version and package identity** (§2) — `packaging/version.sh`, the makefile
   define, `application.hpp`, the Vite define, `/run/qunilator/version`,
   `GET /api/version`, the CI check. Stands on its own and is worth having
   regardless.
2. **The updater and its units** (§6, §7, §13), driven from the command line
   only. Check, install, health check and rollback, provable over ssh on both
   boards before any of it is exposed.
3. **The API** (§11) — the status-file publisher, the event frames, the check
   and install endpoints with their validation.
4. **The System page and the announcement** (§12), install path last.
5. **The reconnect choreography** (§12.2–4), including the version-mismatch
   reload that also covers a manual upgrade.
6. **The OS-upgrade path** (§10), after the emulator update is proven.
7. **`release-image.yml` always passing the repository variables** (§3), the
   unattended-upgrade drop-in, the `DISTRIBUTION.md` correction, and the board
   tests (§14).

## 16. Open questions

- **The repository's availability is now a shipped promise.** Every published
  image points at one host; a board checks daily and reports a failure in the
  interface. Whether that host wants a cache in front of it, and what the
  interface should say about a repository that has been unreachable for weeks,
  is unsettled.
- **`trixie` is written into the source entry** by the image build and into the
  changelog by hand. What a board does when the base OS moves to the next
  Debian release — and who rewrites that entry — has no answer yet.
- **Should the check distinguish security updates** among the other packages?
  apt can be asked, and it changes how loudly the OS row should present itself.
- **What the interface does with a board whose emulator package was installed by
  hand** — `source: none`, so no self-update at all. Offering to write the
  source entry and fetch the keyring would turn those boards into updateable
  ones, and means the interface writing apt configuration.
