# Distributing QBone as a ready-made card image

A plan for a downloadable image that boots a QBone cape into a running
emulator with its web interface up, on Debian 13, and that stays updateable
afterwards through apt.

What the recipient should have to do: write the image, fit the cape, power on,
open `http://qbone.local/`. What they should be able to do later: `apt upgrade`.

## What already exists

Most of the work is done and scattered across three places.

`packaging/build-deb.sh` produces a complete `qbone_*_armhf.deb`: a static
`/usr/bin/qbone` with the PRU firmware inside it, the frontend under
`/usr/share/qunilator/frontend`, both cape overlays in `/lib/firmware`, the
modprobe and modules-load drop-ins, two systemd units, empty state directories
under `/var/lib/qunilator`, and `/usr/share/qunilator/build-ref` - the commit,
branch and clean-or-not state of the tree it was built from, which is how
`qunilator-devkit` finds the sources of a board running a build with no release
tag to its name.

`packaging/debian/qunilator-setup` performs the whole board-side procedure:
rewrites `/boot/uEnv.txt`, checks the hardware, bridges the uplink with an
armed rollback, and starts the services.

`docs/debian-installation.md` records the rest - the settings and masks the cape
needs, and the three ways a stock image wedges a board with a cape fitted.

The gap between that and an image is small and specific: the fixes
`qunilator-setup` does not do (`gpio-manager`, the `ttyGS0` getty, nginx on port
80, the persistent journal), the operator toolset, a user, sample disk images,
and a build that produces the same artifact twice.

## Decision 1: how the image is produced

**A. Golden master.** Set a board up by hand, `dd` the card back, shrink,
publish. Fastest to a first image and reproducible only by memory. Every
subsequent release repeats the whole manual procedure and quietly inherits
whatever else was on that card - shell history, ssh keys, a half-finished
experiment.

**B. Scripted customisation of the rcn-ee base image.** Take
`am335x-debian-13.6-base-v6.12-armhf-*.img.xz`, loop-mount it, chroot in
under qemu, apply a numbered sequence of steps, shrink, emit. One script, one
input image pinned by sha256, output diffable against the last release. This
is what pi-gen and CustomPiOS do for the Raspberry Pi and it is the right
size for this job.

**C. Build from scratch with rcn-ee's `omap-image-builder`.** Total control,
including the bootloader. It also means owning the U-Boot and kernel packaging
that rcn-ee currently does, which is a standing maintenance cost for no
benefit here - the base image's boot chain is already correct for this board.

**D. A declarative image builder** (debos, mkosi, vmdb2). Cleaner than a
shell script and a real dependency to learn and pin. Worth revisiting if B's
script grows past a few hundred lines.

**Recommendation: B.** It reuses the boot chain that is known to work on this
hardware and keeps the delta from stock visible in one file.

Two practical constraints on where it runs. macOS cannot loop-mount ext4, so
the build belongs in a privileged Linux container - the same Docker the cross
build already uses. And on an Apple Silicon host there is no AArch32
execution, so the armhf chroot runs under `qemu-user-static` binfmt
registration rather than natively.

## Decision 2: where the apt repository lives

The image has to ship with a working `sources.list` entry or the "updateable
with apt" half of this does not exist.

**A. Forgejo's Debian package registry.** `code.netzhansa.com` runs Forgejo
15.0.3 and the registry is live - `/api/packages/hanshuebner/debian/repository.key`
already answers. Upload is a `PUT` to
`/api/packages/hanshuebner/debian/pool/{distribution}/{component}/upload`;
Forgejo signs the release and serves the key. Nothing to host, nothing to
sign by hand, and it sits next to the source.

**B. A static repo built with reprepro or aptly**, published to any web
space. Standard, portable, and it makes signing keys and their rotation your
problem.

**C. Releases only, installed with `dpkg -i`.** No upgrade path.

**Recommendation: A**, with the image shipping

    /etc/apt/keyrings/qbone.gpg
    /etc/apt/sources.list.d/qbone.sources   # signed-by the above

pinned to a distribution name of `trixie` and component `main`. B stays the
fallback if the registry's signing turns out to be awkward to consume.

## Decision 3: the sample operating systems

The disk images left this repository in commit `a11c388` and live at
`files.retrocmp.com/qunibone/`. Runtime expects them in
`/var/lib/qunilator/images`, registered through the web interface, each paired
with the `.cmd` script that boots it.

**The scripts came back, the images did not.** `10.03_app_demo/5_applications`
holds one executable command file per example machine again, for both buses, and
`qunilator-devkit` puts them on a card. So the decision below is still open, and
it is now only about the images.

**`qunilator-fetch-images` fetches them in the meantime**, from the example tree
at `files.retrocmp.com/qunibone/10.03_app_demo/`. `tools/fetch-images.py` in the
repository, and the package installs it in `/usr/sbin` beside the other board
commands, because a board that never gets a checkout needs the images just as
much. Nothing about it assumes a repository: with no `qunibone-platform.env` and
no `build.env` to read, the bus comes from which emulator is installed.

    sudo qunilator-fetch-images

**There is one place images live: `/var/lib/qunilator/images`**, the media tree
the web interface already serves, a folder per medium by DEC device mnemonic.
The examples name that path in full and mount exactly what the interface lists;
they used to carry a `diskimages/` directory of their own per tree, which meant
a disk an example booted and the same disk offered in the interface were two
files. There is no second location left to keep in step.

The tool takes one bus's set: `5_applications` plus `5_applications_u` or `_q`,
the same pair `qunibone-platform.sh` merges, unless `--bus` says otherwise. That
is not only about size - both bus trees carry an `rsx11m_4_8_bl70` and they are
different disks, but a machine has one bus, so its media tree takes one of them
under the plain name. 68 images and 0.4 GB for UNIBUS, 56 and 0.5 GB for QBUS,
each filed by medium.

The names on the server are not the ones the command files mount - the same kind
of image is variously `.dsk.gz`, `.img.gz`, `.rk.gz` or `.RL2.gz`, and a few
appear in two directories - so each file is matched against a catalogue in the
script and written as `<medium>/<software>.<disktype>.dsk.gz`. A run is
idempotent: a file already present with the length the server reports is left
alone, so an interrupted fetch resumes, and identical copies of one image are
fetched once.

Two images that could not be in the repository at all are ordinary downloads
here: `rsx11mpv4.6_du1_84.ra80` and `JH_DU1.ra70`, around 300 MB each, were over
GitHub's limit for a single file and had a note standing in for them. A limit on
what a repository holds says nothing about what a board may fetch.

That is a downloader, which takes some of the new code out of option C below,
but it is a command line on the board and not a catalogue the web interface can
offer.

**A. Baked into the image.** Simplest, and it makes every download carry
every sample whether or not it is wanted. It also welds the samples to the
image release cycle.

**B. Separate apt packages** - `qbone-images-rt11`, `qbone-images-xxdp`,
`qbone-images-211bsd`, each dropping into `/var/lib/qunilator/images` with a
matching saved configuration. The base image installs one small sample;
anything else is one `apt install`. Samples then version independently of
the emulator, which is what they want to do.

**C. Fetched on demand by the web interface** from a catalogue URL. The best
experience and the most new code - a catalogue format, a downloader, progress
reporting, checksum handling, and a board that needs internet access to be
interesting.

**Recommendation: B, with one sample baked in.** `xxdp25.rl02` is the natural
resident: it is small, it is the diagnostic monitor, and it is the one image
that carries LQA support, so it doubles as the check that a freshly built
board actually works. RT-11, RSX-11M+, Unix V6 and 2.11BSD become their own
packages.

**This needs a licensing answer before anything is published.** 2.11BSD and
Unix V6 are settled - BSD-licensed and covered by the Caldera release
respectively. RT-11, RSX-11M+ and XXDP are DEC software now owned downstream
of HP, distributed in the hobbyist world under a Mentec-era licence whose
current standing nobody has tested. Serving them from a repository under your
own domain is a more exposed position than a user fetching them from
retrocmp. The conservative split: ship only the freely-licensed images in
packages, and have the web interface point at retrocmp for the DEC ones.
Decide this deliberately rather than by default.

## Decision 4: minimal now, full-sized on first boot

`build-image.sh` shrinks the published artifact and grows it back on the
board's first boot.

    resize2fs -M                    shrink the filesystem to its contents
    sfdisk                          move the partition end down to match
    truncate                        cut the image file

The rcn-ee *base* image carries no first-boot resize of its own (the flasher
images do, this one does not), so `qunilator-resize.service` provides it, in two
stages across a reboot. First boot: extend the partition with `sfdisk`.
Later boot, once the kernel has re-read the enlarged table: grow the ext4
with `resize2fs`, then record `/var/lib/qunilator/.resized`. The two stages
matter - growing the filesystem in the same boot the partition was extended
can leave it a few blocks larger than the on-disk partition the kernel reads
back, which then "exceeds size of device" and will not mount. The
`qunilator-setup` first-boot reboot is the barrier; all tools are from
util-linux, no `growpart`/cloud-guest-utils.

## Emulated Ethernet needs eth0 as a plain NIC (legacy cpsw driver)

The DELQA emulation binds a raw `AF_PACKET` socket to a host interface in
promiscuous mode and carries the guest's several MAC addresses (the setup
filter table plus MOP/DECnet multicast). To reach both the LAN and the
BeagleBone at layer 2 — MOP boots from `mopd` on the BeagleBone, so this must
be L2 — that interface has to be a Linux bridge port over a plain NIC. A
bridge floods unknown-unicast and multicast to the DELQA's veth, so the guest
is reachable at every address it programs.

On the 6.12 kernel the AM335x CPSW binds the switchdev driver
(`ti_cpsw_new`, `cpsw-switch`), which presents `eth0` as a hardware switch
port. The kernel refuses to enslave a switch port to a software bridge
(`ip link set eth0 master br0` → `Invalid argument`), so the bridge cannot be
built. This is the whole difference from the Debian 8 setup, where the legacy
`ti_cpsw` driver presented `eth0` as an ordinary, bridgeable NIC. macvlan is
not a substitute: a macvlan child receives only its own single MAC, and the
one mode that receives all MACs (`passthru`) cannot also reach the host.

Both drivers are built into this kernel. The stock device tree enables the
switch node (`&mac_sw`) and leaves the legacy node (`&mac`) disabled;
`02_bbb_config/01_cape/am335x-boneblack-qbone.dts` flips that — disables
`&mac_sw`, enables `&mac` as a one-slave dual-EMAC CPSW, and wires the PHY.

Two things this took to get right:

- **The file U-Boot loads.** rcn-ee U-Boot picks the base DTB by its own
  `uboot_base_dtb_univ`, not by board-EEPROM detection: on this image it loads
  `/boot/dtbs/<uname_r>/am335x-boneblack-uboot.dtb`, then applies the overlays
  from `uEnv.txt`. The QBone DTB must be installed under that name; the base
  `am335x-boneblack.dtb`, `-revd`, etc. are never loaded. `dtb=`/`fdtfile=` in
  `uEnv.txt` do not override this while `enable_uboot_overlays=1`.
- **The `-uboot` base.** `am335x-boneblack-qbone.dts` includes
  `am335x-boneblack-uboot.dts`, not the plain `am335x-boneblack.dts`. The plain
  variant pulls in HDMI/audio, whose McASP claims PIN100 — the PRUSS needs it,
  and the PRU cores then fail to probe (`pin PIN100 already requested by
  48038000.mcasp`), which stops the emulator. The `-uboot` variant has no
  McASP, so the PRU pins are free.

Build it from the on-board device-tree sources (present as
`/opt/source/dtb-6.12.x`):

    cd /opt/source/dtb-6.12.x
    cp .../am335x-boneblack-qbone.dts src/arm/ti/omap/
    make ARCH=arm CPP=cpp DTC=dtc src/arm/ti/omap/am335x-boneblack-qbone.dtb
    cp src/arm/ti/omap/am335x-boneblack-qbone.dtb \
       /boot/dtbs/<uname_r>/am335x-boneblack-uboot.dtb   # keep a .stock backup

### The bridge, in systemd-networkd

The image manages the network with `systemd-networkd`, so the bridge is built
there, not in ifupdown (an `/etc/network/interfaces.d` stanza fights networkd
and neither wins). The files are in `packaging/debian/network/`:

    br0.netdev     bridge; MACAddress pinned to the uplink by qunilator-setup
    br0.network    DHCP on br0, ClientIdentifier=mac
    eth0.network   eth0 is a bridge port, no address
    veth-br.network   the controller veth's far end is a bridge port
    veth-pdp.network  the DELQA's end: up, no address

`qunilator-network` creates the `veth-pdp`/`veth-br` pair; networkd enslaves
`veth-br` and `eth0` to `br0`, and `br0` holds the host's DHCP address. The
DELQA defaults its `interface` parameter to `veth-pdp`. networkd bridges
default to STP off, so ports forward immediately and DHCP completes at boot.

Verified on hardware: `eth0` a plain NIC, `br0 = eth0 + veth-br` holding the
host address, the DELQA bound to `veth-pdp` in promiscuous mode. The remaining
check is an end-to-end guest test (2.11BSD `qe0` pinging the BeagleBone and a
LAN host).

**Not yet automated in the package**: building and installing the DTB, pinning
`br0`'s MAC in `qunilator-setup`, and shipping the networkd files in place of the
ifupdown bridge. The artifacts and procedure above are the reference for that
work.

## What the image build applies

In order, in the chroot:

1. **Boot settings.** `disable_uboot_overlay_emmc=1`, `disable_uboot_overlay_video=1`,
   `disable_uboot_overlay_audio=1`, `uboot_overlay_pru` commented out,
   `uboot_overlay_addr4=QBone.dtbo`. `qunilator-setup` already writes exactly
   this; the image build can call it or share its code rather than
   duplicating the edits.
2. **Packages.** Free port 80 for the web interface: `apt purge nginx
   nginx-common libnginx-mod-http-fancyindex cockpit-ws cockpit-system
   cockpit-packagekit`, and remove the orphaned `/var/www/html/Cockpit.html` -
   the emulator binds 80 and will not start behind nginx. Then install the
   operator toolset the appliance is run and debugged with: `apt install gdb
   tcpdump zsh tmux ckermit`. These belong to the image, not the qbone
   package, which stays limited to what the emulator and its setup script
   need.
3. **`systemctl mask gpio-manager.service`** and `apt-mark hold gpiod`, or
   purge `gpiod` outright - nothing in the image needs it.
4. **`systemctl mask serial-getty@ttyGS0.service`**, which is the failure
   that presents as a broken systemd.
5. **Persistent journal** - `mkdir /var/log/journal`.
6. **Passwordless root and the ssh policy.** `passwd -d root` gives the
   physical console a passwordless root login, and a
   `/etc/ssh/sshd_config.d/10-qbone.conf` drop-in denies root logins and
   empty-password logins over ssh (`PermitRootLogin no`, `PermitEmptyPasswords
   no`), so that empty password never reaches the network. Ordinary password
   logins - the base image's `debian` account - still work for onboarding.
   `sshd_config` includes the drop-in directory before its own body and takes
   the first value for each keyword, so the drop-in wins. Personal accounts
   are not baked in; `personalize-image.sh` adds one to a copy of the image.
7. **Install the `qbone` package** and enable the units. The image enables
   `qunilator-setup.service` as well, so the board runs `qunilator-setup --auto` on
   first boot and configures its network bridge with no login - the one step
   that cannot be baked, since the bridge pins the board's own uplink MAC. It
   reboots itself between the setup passes and records
   `/var/lib/qunilator/.setup-done` when finished. A package install leaves the
   unit disabled, where the operator drives `qunilator-setup` by hand. The image
   also enables `qunilator-leds.service`, the status-LED indicator.
8. **The apt repository** keyring and sources entry.
9. **Identity reset**, last: truncate `/etc/machine-id` to zero bytes, remove
   `/etc/ssh/ssh_host_*`, clear shell history, apt lists and logs, and set
   the hostname to `qbone` so `qbone.local` resolves over mDNS. A published
   image that ships one set of ssh host keys gives every board that identity.
10. **Kernel pin.** `apt-mark hold linux-image-6.12.93-bone63`. 6.12 is the
    newest kernel with an RT build and the one the overlay and pinmux are
    verified against; an unattended upgrade that moves off it changes the
    thing the whole port depends on.

## Package changes, done

- **The units are enabled on install.** A first install enables both without
  starting them, since the emulator needs boot settings `qunilator-setup` applies
  and a reboot to pick them up. An upgrade restarts whatever was running and
  leaves a disabled unit disabled.
- **`Depends` names the tools the scripts call** - `iproute2`, `ifupdown`,
  `bridge-utils`. The field is written by `build-deb.sh`, which assembles the
  binary control file field by field; `packaging/debian/control` is not read
  for it, and its `${misc:Depends}` is a debhelper substitution this build
  does not perform.
- **Version 1.6.0-1, distribution `trixie`.**
- **A `postrm` disables the units on removal.**

## The build pipeline

    crossbuild.sh      →  PRU firmware + demo binary
    build-deb.sh       →  qbone_*_armhf.deb
    build-image.sh     →  qbone-dist.img (a card-ready appliance image)

`packaging/build-release.sh` drives all three on an x86_64 Linux workstation, so
a release image is one command from a clean checkout. It stages what
`build-image.sh` wants under `dist/`: it downloads the pinned rcn-ee base image
and checks it against the published `.sha256sum` when `dist/base.img.xz` is not
already there, builds and copies the package, and fetches the disk images and
boot configurations from `ASSETS_URL` when `dist/images` and `dist/configs` are
missing. `-u` builds the UNIBUS board's image, `-p` reuses the package already
staged, `-x` compresses the result. Set `APT_REPO_URL`/`APT_REPO_KEY_URL` for an
image whose board can update itself; without them the script warns and builds
one that cannot. Nothing already staged is fetched twice - delete the file or
the directory to refresh it.

`build-image.sh` turns the rcn-ee base image into the appliance. It takes a
staging directory holding the base `base.img.xz`, the `qbone_*_armhf.deb`, an
`images/` directory of disk images to ship, and a `configs/` directory of boot
configurations that name them, and produces a flashable `.img`. All of the
Linux work - loop-mounting the ext4 root, the armhf chroot, the resizes - runs
in a privileged Docker container, since macOS can do none of it; on Apple
Silicon the chroot runs under qemu-user-static. The script grows the root to
make room, installs the package and the operator toolset, removes nginx and
cockpit, builds and installs the legacy-Ethernet device tree, applies the
uEnv boot settings, places the operating systems and their configurations
under `/var/lib/qunilator`, resets the machine identity, enables the services, and
shrinks the filesystem back to fit. The bridge MAC is not baked in: it is
pinned to the board's own uplink by `qunilator-setup` at first boot. Write the
result to a card with `dd`; rcn-ee's first-boot resize grows the root to fill
it.

`crossbuild.sh` used to fetch the PRU firmware from a live board over scp,
because `clpru` only exists there, which made a release build depend on a
particular BeagleBone being powered on. It now builds the firmware in a
container of its own: TI's code generation tools, pinned to **2.3.1**, the
version that built the firmware the emulator has been verified with. `clpru`
is an x86-64 binary inside an i386 installer, so that container is amd64
whatever the host is and carries a 32-bit C library to unpack it. The
`prussdrv` headers, formerly scp'd from the board as well, are in the tree.

A build therefore needs this repository and Docker, and nothing else. Three
workflows drive it:

- `.github/workflows/build.yml` runs on every push and pull request:
  cross-compile both platforms, rebuild the firmware from clean and require the
  two builds to agree, package, and check the maintainer scripts parse.
- `.github/workflows/release-deb.yml` runs on a `v*` tag: build both boards'
  packages, check the tag matches the changelog version, attach both `.deb`s to
  the tag's GitHub Release, and push them over an ssh forced command to the apt
  repository host, which drops them into `pool/main` and rebuilds the signed
  index (skipped unless the deploy key and host are configured).
- `.github/workflows/release-image.yml` runs on a `v*` tag and can be dispatched
  by hand: it builds each board's package, downloads the base image and the
  OS-images/configs assets from repository variables, runs `build-image.sh`, and
  attaches the compressed appliance images to the same Release. It requires the
  apt repository variables, so no image ships a board that cannot update itself.

**The firmware this produces is not bit-identical to what the board built.**
Same compiler version, three extra instructions, all in the C runtime startup
region - the board's `clpru` came from the BeagleBoard Debian package and TI's
own installer evidently ships a different `libc.a`. Two container builds agree
with each other, so the build is reproducible; what is unverified is the new
firmware against real hardware. **Run the diagnostics on a live machine before
any of this is released.**

Both boards' packages are published: `qbone` for QBUS, `unibone` for UNIBUS,
each named for its board and mutually exclusive with the other through a
`Conflicts`/`Replaces` pair, so they share one repository and a board is only
ever offered the brand it has.

## Where machines come from

A machine is a saved configuration and the media its drives hold, and the two
travel together: the web interface exports `<name>.qcfg.zip`, holding the
configuration document and every image it names, and imports the same zip back.
So a machine is a file an operator can be handed, and a board acquires one
without anything having been baked into its image.

The appliance image therefore carries no operating systems. What a freshly
flashed board has is what its package ships: the **XXDP diagnostic pack**
(`xxdp25.rl02`, stored compressed in `packaging/images/` and the one binary in
this repository that is not a build product) and a configuration that attaches
it. XXDP earns the space by being the pack that says whether a board works at
all. On a UNIBUS build that configuration is a whole emulated PDP-11/20 with the
M9312 ROMs to boot the pack; a QBUS board is a peripheral of a real machine,
which brings its own processor and console, so there it is the drive alone.

The pack is read-only and belongs to the package. A drive takes its guest's
writes into a copy-on-write overlay, so the shipped file stays as it was and a
reinstall cannot lose an operator's work.

Everything else is imported. [Issue #64](https://github.com/QUniBone/QUniLator/issues/64)
covers doing that from the interface: a board subscribes to several catalogues -
the project's, a user group's, an operator's own - and imports what one lists.
Until then an operator is handed a zip and feeds it to the import dialog.

### What this settles about the licensing

Decision 3 asked which sample operating systems ship and under what licence. The
answer is that the distributed image ships one, and it is the same DEC
diagnostic pack the boards have always carried. Everything with a less certain
standing - RT-11, RSX-11M+, and VAX/VMS most of all, which VSI licenses today
and runs its own community programme for - is no longer something a download
carries whether or not it was wanted. It becomes a configuration an operator
goes and gets, which is a materially better position than serving it to
everyone who flashes a card.

What a published catalogue may carry is the same question in a smaller place,
and it is still open.

## Self-update, done

The web interface tells the operator when a newer package is published, shows the
candidate's own changelog since the installed version, and installs it. The
mechanism is the apt repository every published image is pointed at:
`apt-get update` and `apt-cache policy` for the check, `apt-get install` for the
install, so apt verifies the signed index and the package hash and there is no
unsigned install path.

`/usr/sbin/qunilator-update` is the whole board side, and the only thing that
runs apt. It resolves its own brand from the package that owns it, so one script
serves both boards unrebranded. The interface never runs apt: it writes the
requested version to a file and starts `qunilator-update.service`, which lives in
its own systemd cgroup — the install restarts the emulator's unit, and a dpkg
running as a child of that unit would be killed with it, mid-install.

An install is watched: the new service must go active, answer on the loopback,
publish the new version in `/run/qunilator/version`, and still be there ten
seconds later. Failing that, the cached previous package is reinstalled and the
board comes back on the version it had. `qunilator-update-check.timer` checks
daily, and an `apt.conf.d` drop-in keeps unattended-upgrades away from the
emulator package, which would otherwise stop a running machine with no warning.

The operator's page rides the restart out and reloads onto the matching bundle.
Any page whose bundle version differs from what the server reports reloads, so a
hand-run `apt upgrade` over ssh carries every open tab across too.

**The repository's availability is now a shipped promise.** Every published image
points at one host and every board checks it daily, reporting a failure in the
interface. Whether that host wants a cache in front of it is unsettled.

The board's other packages are reported by the same check and can be upgraded
from the interface. That path has no rollback — apt cannot undo an upgrade — so it
holds the emulator package back, uses `upgrade` rather than `full-upgrade`, leaves
the kernel held, and never reboots the board.

## Web interface authentication, done

An installation nobody has set up answers every request, which is what makes a
fresh one reachable. The frontend asks for the operator's name and password the
first time it reaches one in that state, in a dialog with no cancel, and PUTs
them to `/api/auth`; from then on every request needs both, static files and
WebSocket handshakes included.

The password is kept in `settings.json` as a PBKDF2-HMAC-SHA256 digest over a
random salt, at 120000 iterations. The build links no crypto library - it is
static and civetweb is compiled `-DNO_SSL` - so SHA-256, HMAC and PBKDF2 are
in `webauth.cpp`, checked against the FIPS 180-4, RFC 4231 and RFC 7914
vectors. `settings.json` now carries a credential, so it is written through a
private temporary and renamed, mode 0600.

Basic auth resends the password on every request and PBKDF2 costs more than
serving the page, so a password that verifies once is remembered as a single
SHA-256 over a salt generated afresh at each start. One request per run of
the process pays the full derivation.

Credentials are a name and a password together, and settings.json is the only
place they come from. The name is an OS account as well, so one identity opens
the web interface, the file shares and ssh; the `qunilator` service account
takes no login of its own.

**The window is narrower, not closed.** Between first boot and someone setting
the operator up, the interface is open to anyone who can reach it. Preparing
the SD card closes it entirely, in either of two ways:

- `packaging/personalize-image.sh` runs the emulator's own `--setup-operator`
  inside the image on a Linux workstation, so the card is written already set
  up.
- A `qunilator.toml` on the card's FAT `BOOT` partition, which any workstation
  mounts and any text editor writes. `qunilator-seed.service` applies it before
  the emulator starts and deletes it. `webseed.cpp` reads it, and the format is
  documented in the manual's install page; the same file written later is how
  an operator whose password is gone gets back in.

The seed carries the password either in the clear, which is what a hand-written
card does, or already derived into the three shapes the one identity is checked
in — a PBKDF2 digest for the web interface, a `crypt(3)` hash for the Linux
account, an NT hash for Samba. No single hash yields the other two, so the
derived form carries all three or the file is refused. `chpasswd --encrypted`
takes the first, and Samba takes the third through an entry made with a
throwaway password and then `pdbedit --set-nt-hash`, since `smbpasswd` will not
add an entry without one.

## Open questions

- **Does the firmware built here behave on real hardware?** It differs from
  the board's own build in the C runtime startup. Nothing else in this plan
  matters until that is answered on a live machine.
- Does the base image's rootfs growth survive the shrink, and which unit
  performs it?
- RT or non-RT kernel in the shipped image? The port runs on the non-RT
  `6.12.93-bone63` against a live 11/73 and whether RT is needed at all is
  still unmeasured. Shipping non-RT keeps the measurement honest.
- Does the image ship with the uplink already bridged, or does it leave that
  to a first `qunilator-setup` run? Bridging at build time cannot arm the
  rollback that makes it safe, which argues for leaving it.
- The apt repository host. CI is GitHub Actions, matching this repository's
  `origin`; the Debian registry on `code.netzhansa.com` is verified working
  and is where the publish step points, which splits the two across hosts.
