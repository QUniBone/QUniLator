# Installing Debian on a QBone BeagleBone

Setting up a card from scratch, for a BeagleBone Black carrying a QBone or
UniBone cape. Written while moving from Debian 8 to Debian 13, and updated as
the remoteproc port proceeds.

## Choosing an image

Two things constrain the choice, and they pull in opposite directions.

**PREEMPT_RT** is now mainline, but that only means the code is merged - it is
still a build-time option, and a kernel has to be compiled with
`CONFIG_PREEMPT_RT=y`. `PREEMPT_DYNAMIC` switches between none, voluntary and
full at boot; it does not reach RT. The BeagleBone RT builds stop at 6.12:

    linux-image-6.12.93-bone-rt-r63     newest RT
    linux-image-6.12.93-bone63          same kernel, CONFIG_PREEMPT only

Nothing `-rt` exists for 6.18, 6.19 or 7.x.

**uio_pruss** was removed from mainline in 6.10, so `libprussdrv` works up to
6.6 and not beyond. That ceiling disappears once the emulator reaches the PRUs
through remoteproc; see `docs/plans/remoteproc-port-plan.md`.

The image used here:

    https://rcn-ee.com/rootfs/debian-armhf-13-base-v6.12/2026-07-15/
    am335x-debian-13.6-base-v6.12-armhf-2026-07-15-4gb.img.xz

Debian 13.6 with the 6.12 kernel: the newest Debian, and the newest kernel
that an RT build exists for. It ships the non-RT `6.12.93-bone63`, which is
deliberate - whether the emulator needs RT at all is unmeasured, and the
non-RT kernel is the baseline that answers it. Installing the RT variant
later is one `apt install` and a reboot, with no other change.

## Writing the card

    xz -dc am335x-debian-13.6-*.img.xz | sudo dd of=/dev/rdiskN bs=4m

Check the image against its published `.sha256sum` first, and check the target
device is the one you mean: external, removable, and the size you expect. An
`if`/`of` typo here overwrites a disk that matters.

Two ways to be misled when verifying afterwards, both of which look exactly
like a corrupt card:

- **macOS mounts a freshly written FAT partition on its own** and writes
  Spotlight metadata into it, so a whole-device comparison can never match
  after a write. Compare the ext4 root partition instead, which macOS cannot
  mount, or compare before anything has had a chance to mount.
- **`dd skip=` on a pipe miscounts.** A short read from a pipe still counts as
  a block, so `xz -dc image | dd skip=N` skips the wrong distance. Use
  `tail -c +$((offset+1)) | head -c $length`, which counts bytes.

## Editing the card before first boot

`/boot/uEnv.txt` lives on the **ext4 root partition**, not the FAT one. The FAT
partition holds `ID.txt`, `START.HTM` and `sysconf.txt` and nothing that
matters here.

macOS cannot mount ext4. `brew install e2fsprogs` gives `debugfs`, which edits
the filesystem directly:

    debugfs -R "dump /boot/uEnv.txt ./uEnv.txt" /dev/diskNs3   # read
    debugfs -w -R "rm /boot/uEnv.txt" /dev/diskNs3             # replace
    debugfs -w -R "write ./uEnv.txt boot/uEnv.txt" /dev/diskNs3

The image ships with an unreplayed journal, so `debugfs` refuses to open it
until `e2fsck -f -y /dev/diskNs3` has run. That is what the kernel would do on
first mount anyway. Run `e2fsck -f -n` afterwards to confirm the edit left the
filesystem consistent.

## Settings the cape needs

### Disable the onboard eMMC

**Required.** The cape claims P8.3, P8.4, P8.5 and P8.6 for its switches and
LEDs, and P8.20/P8.21 further down. On a BeagleBone Black those are the eMMC
data, command and clock lines. With the cape fitted and the eMMC probed, the
SDHCI controller takes spurious interrupts and the driver dereferences
garbage:

    mmc1: Got data interrupt 0x00000002 even though no data operation
          was in progress.
    Unable to handle kernel paging request at virtual address fffffffc
    Internal error: Oops: 27 [#21] PREEMPT THUMB2

Note `mmc1` - the eMMC. `mmc0` is the microSD the system boots from, and a
card fault would name that instead. In `/boot/uEnv.txt`:

    disable_uboot_overlay_emmc=1

The eMMC is unusable while the cape is fitted, which was already true.

### Mask the GPIO manager

**Required.** The `gpiod` package from the BeagleBoard repository ships
`gpio-manager.service`, a daemon that claims every GPIO chip at startup. With
the cape fitted it never finishes starting, and the boot stops at

    Starting gpio-manager.service - Centralized GPIO manager daemon...

Nothing else runs, because systemd is still waiting for it.

    systemctl mask gpio-manager.service

Whether it is a regression in a young package or a genuine conflict with the
cape driving those lines is not established; the version that did this was
three days newer than the image. Either way the emulator does not want it -
it reaches its pins through the PRUs and /dev/mem, and a userspace manager
claiming the same lines is at best redundant. Consider `apt-mark hold gpiod`
so an unattended upgrade cannot reintroduce it.

### Mask the USB gadget getty

**Required**, and the nastiest of the three, because it presents as "systemd
is broken" rather than as anything to do with a getty.

The image runs a login prompt on the USB gadget serial port, `ttyGS0`. Log out
of the real serial console once and the machine becomes unusable: no new login
prompt appears, ssh accepts a connection and prints its banner but never
completes a login, `sudo` hangs, and `systemctl` itself hangs.

What is happening is that **PID 1 is blocked in `flock()`**:

    # cat /proc/locks
    1: FLOCK  ADVISORY  WRITE 586 00:06:11 0 EOF
    1: -> FLOCK  ADVISORY  WRITE 1 00:06:11 0 EOF

The waiter is systemd itself, and the holder is the `ttyGS0` agetty, which
holds `/dev/console` open. Restarting the console getty needs a lock that
agetty already has, so systemd blocks, and every job queues behind it -
including its own shutdown, so the machine will not even reboot.

    systemctl mask serial-getty@ttyGS0.service

Mask it *and* kill the running agetty; masking alone leaves the current
process holding the lock. With PID 1 already blocked, nothing else will
complete either, so the order matters:

    kill $(pgrep -x agetty)      # unwedge PID 1 first
    systemctl mask serial-getty@ttyGS0.service

Little is lost. The gadget also provides a network interface, so console
access over the same USB cable is `ssh 192.168.6.2`.

### Disable the PRU overlay

**Required.** The image enables the UIO overlay by default:

    uboot_overlay_pru=AM335X-PRU-UIO-00A0.dtbo

On 6.12 this applies cleanly and takes the PRUs away. Its second fragment
overwrites the PRU-ICSS `compatible` with `ti,pruss-v2`, which no driver has
bound since `uio_pruss` was removed in 6.10, so `pruss` never probes and no
`/dev/uio*` appears either. Comment the line out and the base tree's own PRU
hierarchy comes through:

    remoteproc1  4a334000.pru
    remoteproc2  4a338000.pru

### Disable the video and audio overlays

**Required.** The cape claims the HDMI and audio pins - P8.27 to P8.46 for
the PRU register bus, P9.25 to P9.31 for the PRU data outputs. Left enabled,
`lcdc` and `mcasp0` claim them first and the cape overlay's pinmux fails,
taking the whole PRU-ICSS down with it:

    pin PIN100 already requested by 48038000.mcasp; cannot claim for 4a300000.pruss
    pruss 4a300000.pruss: Error applying setting, reverse things back

In `/boot/uEnv.txt`:

    disable_uboot_overlay_video=1
    disable_uboot_overlay_audio=1

### Cape device tree overlay

`bone_capemgr` was removed in 4.19, so the overlay no longer loads from the
cape's EEPROM. `02_bbb_config/01_cape/QBone.dtso` is the current source,
applied by U-Boot:

    dtc -@ -I dts -O dtb -o QBone.dtbo QBone.dtso
    cp QBone.dtbo /lib/firmware/

and in `/boot/uEnv.txt`, a bare filename - U-Boot looks in `/lib/firmware`:

    uboot_overlay_addr4=QBone.dtbo

Beyond the cape's own pins it declares the two things `uio_pruss` used to
supply from the kernel side: an 8 MB `reserved-memory` region named
`qbone-ddr` for the emulated machine's memory, and a `generic-uio` node bound
to PRU system event 19, which is what the firmware raises with
`R31 = 0x20 | 3`.

Three things about the syntax, each of which fails quietly rather than loudly:

- **Three cells per pin, not two.** The AM335x pinmux now declares
  `#pinctrl-cells = <2>`, so each entry is `<offset conf mux>` where the old
  combined value splits into its pull/input bits and its mode. A two-cell
  entry still compiles, and pinctrl-single reads every second mux value as if
  it were an offset - the group ends up naming pins nobody meant. `dtc` says
  nothing; the evidence is in
  `/sys/kernel/debug/pinctrl/44e10800.pinmux-pinctrl-single/pingroups`, where
  a correct group lists exactly the pins it declares.
- **A pin group needs a consumer.** Nothing applies a group for its own sake,
  and `bone-pinmux-helper` no longer exists. Muxing is global, so one device
  can carry the whole group: the cape's GPIO pins hang off `&gpio1`.
- **Nodes with a `reg` carry a unit address**, so the reserved region is
  `qbone-ddr@9f000000` in `/proc/device-tree` and code looking for it has to
  match the prefix.

### Keep the GPIO modules clocked

The emulator reaches the GPIO registers through `/dev/mem`, which bypasses
runtime PM. A bank whose driver has let it idle has no clock, and an access
to it aborts:

    481ae000.gpio: control=auto status=suspended
    Unhandled fault: external abort on non-linefetch (0x1018)

The overlay sets `ti,no-idle` on each bank's interconnect target module,
which holds the module enabled whatever the GPIO driver does. Check with

    cat /sys/bus/platform/devices/481ae000.target-module/power/runtime_status

which should read `active`. The `.gpio` device below it may still read
`suspended`; that is the driver, not the clock.

### Bind uio_pdrv_genirq

The driver matches no compatible of its own - the one it looks for comes from
a module parameter, so the overlay's `generic-uio` node binds to nothing until
it is told:

    echo 'options uio_pdrv_genirq of_id=generic-uio' > /etc/modprobe.d/qbone.conf
    echo uio_pdrv_genirq > /etc/modules-load.d/qbone.conf

`/dev/uio0` then appears, named `qbone-pru-event`, and

    grep qbone /proc/interrupts

shows it on `pruss-intc 19 Edge` with a count that rises while the emulator
runs.

## First boot

The root filesystem expands to fill the card, so a 4 GB image on an 8 GB card
ends up with an 8 GB root. Nothing to do, but it changes the partition sizes
from what was written.

## Worth doing early

**Make the journal persistent.** These images keep it in memory, so
`journalctl -b -1` is empty after a reboot - which is exactly when it is
wanted.

    mkdir -p /var/log/journal
    systemctl restart systemd-journald

**Check where NOPASSWD lands.** `/etc/sudoers` ends with
`@includedir /etc/sudoers.d`, and sudoers is last match wins, so a rule in the
main file loses to anything the image ships in that directory. The image ships
`/etc/sudoers.d/admin`:

    %admin ALL=(ALL:ALL) ALL

and puts the first user in the `admin` group, so that rule wins over any
NOPASSWD line in `/etc/sudoers` and sudo asks for a password anyway.

Files in `sudoers.d` are read in lexical order, so the fix has to sort after
`admin` - and a numeric prefix does not: `9` is 0x39 and `a` is 0x61, so
`99-user` is read *before* `admin` and loses to it.

    echo 'user ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/zz-user-nopasswd
    chmod 0440 /etc/sudoers.d/zz-user-nopasswd
    visudo -c

`sudo -l` prints the rules as they actually apply, which settles it in one
line; the last entry should carry NOPASSWD.

**Turn off the web console** to leave port 80 free. The image runs nginx on
port 80 serving `/var/www/html`, which holds a single `Cockpit.html` that
redirects to `https://<host>:9090/`. Cockpit itself listens on 9090 through
`cockpit.socket`, socket-activated by systemd.

    systemctl disable --now nginx
    systemctl disable --now cockpit.socket

Both are manually installed with nothing depending on them, so purging works
too:

    apt purge cockpit-ws cockpit-system cockpit-packagekit
    apt purge nginx nginx-common libnginx-mod-http-fancyindex
    apt autoremove --purge

`/var/www/html/Cockpit.html` belongs to no package and stays behind, as does
the image's own mask symlink for `cockpit-issue.service`.

**Silence the ssh banner** if it gets in the way. It is sent before
authentication, so `scp`, `rsync` and `git` over ssh all print it:

    sed -i 's/^Banner/#Banner/' /etc/ssh/sshd_config

## When it wedges

`diagnostics/bbb-diag.sh` collects what a hung system will still say. Run it as
root while the machine is misbehaving; everything that talks to systemd is
wrapped in a timeout, so a blocked PID 1 cannot hang it, and those sections
reporting a timeout is itself the finding.

The order that led to the answer here, and would again:

1. `mount` and `dmesg` for filesystem or storage errors - if the root
   filesystem has gone read-only or the card is throwing I/O errors, every
   other symptom is downstream and there is no point reading further.
2. Processes in **D** state, and `/proc/1/stack`. A PID 1 in uninterruptible
   sleep is blocked in the kernel, usually on I/O.
3. `/proc/locks`. A `->` prefix marks a waiter; if PID 1 is one of them, the
   holder's pid is in the same line and `ps` names it.

Two dead ends worth not repeating: the interrupted `apt upgrade` and the
`Transport endpoint is not connected` errors it printed were consequences of
a blocked PID 1, not a cause, and neither memory nor the SD card was ever
implicated - no OOM, no I/O errors, clean `dpkg --audit`.
