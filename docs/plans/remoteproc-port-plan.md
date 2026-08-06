# Porting the PRU interface from prussdrv to remoteproc

The emulator reaches the PRUs through `libprussdrv`, which is built on the
`uio_pruss` kernel driver. That driver was removed from mainline in **6.10**,
by TI, on the grounds that remoteproc supersedes it. Nothing brings it back:
a custom kernel build cannot enable a driver whose source is gone.

That, not the realtime patch, is what pins the machine to an old kernel.
PREEMPT_RT went the other way and is now mainline, so realtime support
improves with newer kernels while `uio_pruss` support ends.

Measured, by extracting the shipped kernel packages:

| kernel | `uio_pruss.ko` | PREEMPT_RT |
|---|---|---|
| `5.10.168-ti-rt-r84` | yes | yes |
| `6.1.83-ti-rt-r42` | yes | yes |
| `6.6.142-bone-rt-r47` | yes | yes |
| `6.12.93-bone-rt-r63` | **no** | yes |

A trap worth knowing: the 6.12 packages still ship
`AM335X-PRU-UIO-00A0.dtbo`, and `uboot_overlay_pru=` still applies it. No
driver binds it, so no `/dev/uio*` appears and the overlay looks configured
while doing nothing.

## What the port has to replace

`libprussdrv` is used in one file, `10.01_base/2_src/arm/pru.cpp`, plus one
call in `qunibusadapter.cpp`. Four distinct jobs:

**Loading firmware.** `prussdrv_exec_code_at()` writes raw instruction words
into instruction memory and starts execution at an address.

**Mapping PRU memories.** `prussdrv_map_prumem()` for the PRU data memories,
which carry the mailbox and the device register descriptors.

**Mapping host memory.** `prussdrv_map_extmem()`, `prussdrv_extmem_size()`
and `prussdrv_get_phys_addr()` hand out the DDR region the PRUs use as the
emulated machine's memory. `uio_pruss` reserves that region itself.

**Waiting for PRU events.** One call, in `qunibusadapter.cpp`:
`prussdrv_pru_wait_event_timeout(PRU_EVTOUT_0, 100000)`.

## What it does not have to replace

The mailbox is **not** an interrupt protocol. ARM and PRU hand work to each
other by writing a shared memory word and spinning on it
(`mailbox->arm2pru_req`). That needs the memory mapped and nothing else, so
it survives the port untouched.

## How each job maps

**Firmware** is already remoteproc-shaped, which is the pleasant surprise
here. The build produces `.out` ELF files and derives the C arrays from them
with `hexpru --array`; every firmware already includes
`resource_table_empty.h`, and `AM335x_PRU.cmd` places `.resource_table`.
remoteproc wants exactly that: an ELF carrying a resource table. Both entry
points are `0x00000000`, so the ELF entry serves and nothing needs an
explicit start address.

So embed the `.out` files rather than the instruction arrays, write them
where the kernel looks for firmware at startup, name them through
`/sys/class/remoteproc/remoteprocN/firmware` and write `start` to `state`.
The executable stays self-contained: the files it writes are its own
contents, regenerated on every run.

**PRU memories** become `/dev/mem` mappings at the documented physical
addresses. Mechanical.

**Host memory** is the one with no drop-in replacement. Without `uio_pruss`
there is no reserved pool, so the region has to be declared in the device
tree as `reserved-memory` and mapped through `/dev/mem`. Its physical
address must be known to both sides, as it is today.

**PRU events** keep a UIO device, just not that one. `uio_pdrv_genirq` is
generic, still in mainline, and binds to an interrupt named in the device
tree - a PRU host interrupt among them. It gives back a `/dev/uioX` whose
blocking `read()` returns on each event, which is the semantics the current
call already has.

## Order of work

1. **Put a backend behind an interface.** Move the prussdrv calls behind a
   small interface with the existing implementation as its first backend.
   Testable on the current machine: the emulator has to keep working
   unchanged.
2. **Embed ELF firmware.** Keep `hexpru --array` for the prussdrv backend and
   add byte arrays of the `.out` files for the other one.
3. **Write the remoteproc backend.** Done, and running: on Debian 13 it
   selects itself, finds both PRUs and maps their memories. It stops at the
   host memory region, which the device tree does not yet declare, and says
   so by name.
4. **Enable the PRUs.** Done, by *removing* an overlay rather than adding
   one. The base DTB already carries the whole hierarchy - `pruss_tm` is
   `status = "okay"`, `pruss@0` is `ti,am3356-pruss`, and `pru@34000` and
   `pru@38000` are `ti,am3356-pru` - so the only thing in the way was the
   shipped UIO overlay, whose second fragment overwrites `compatible` with
   `ti,pruss-v2`. Nothing binds that since 6.10, so the PRUs vanished.

   Comment out `uboot_overlay_pru=` in `/boot/uEnv.txt` and they appear as
   `remoteproc1` and `remoteproc2`, named `4a334000.pru` and `4a338000.pru`.

   The drivers are all present as modules (`CONFIG_PRU_REMOTEPROC`,
   `CONFIG_TI_PRUSS`, `CONFIG_TI_PRUSS_INTC`), and `CONFIG_STRICT_DEVMEM` is
   not set, so mapping the PRU memories through /dev/mem still works.
5. **The cape overlay.** Done, as `02_bbb_config/01_cape/QBone.dtso`. It
   declares the `reserved-memory` region the emulated machine's memory lives
   in, binds PRU system event 19 to `uio_pdrv_genirq` for events, claims the
   cape's pins, and sets `ti,no-idle` on each GPIO bank's target module so
   the modules stay clocked for /dev/mem access.

   `docs/debian-installation.md` carries the details that cost time: three cells
   per pin rather than two, a pin group needing a consumer device now that
   `bone-pinmux-helper` is gone, and the video and audio overlays having to
   be disabled before the cape can have its pins.
6. **GPIO numbering.** Done. The legacy sysfs base moved from 0 to 512, so
   the hardcoded numbers named nothing; each bank now asks the chip whose
   label covers its range. Moving to /dev/gpiochipN and libgpiod remains the
   durable answer.

## Where it stands

On Debian 13.6 with 6.12.93-bone63 the emulator selects the remoteproc
backend, maps the PRU memories and the reserved region, writes both firmware
ELFs from its own contents and boots them, and receives PRU events:

    remoteproc1: Booting fw image qbone-pru0.elf, size 32668
    remoteproc2: Booting fw image qbone-pru1.elf, size 118192
    61:  2  pruss-intc  19 Edge  qbone-pru-event

Against the live 11/73 - CPU card, 4 MB memory card, QBone - 22-bit DATI
cycles read the memory card across its whole range, so arbitration and the
bus master path work: address 0, and 17000000 just under the 4 MB top, both
answer. 17777776 times out, which is right; the J11's PSW is not reachable by
DMA.

That test also found the one defect the port introduced. `uio_pdrv_genirq`
requests its interrupt with `IRQ_NOAUTOEN`, so it stays disabled until
userspace writes to `/dev/uioX`. Arming it only in `clear_event()`, after the
first wait, lost the first event and with it the first bus transaction: the
first DATI after entering device emulation timed out every time, and the same
read immediately after succeeded. The event device is armed when it is opened.

## Does the stock kernel need to be a realtime one

The emulation depends on the kernel in one place, and `/api/latency`
measures it: the PRU leaves RPLY asserted from raising a device register
event until the ARM acknowledges, so that interval is a QBUS cycle held open
while Linux schedules the bus worker. It is a stall, not a failure - the PRU
answers the cycle from its own copy of the register before the ARM is
involved, so the bus timeout is long satisfied and what the ARM holds open
is the tail of an already-answered cycle.

Three hours on the non-RT `6.12.93-bone63`, 2.11BSD multi-user driving the
emulated RA81, 66506 events:

| bucket | events | share |
|---|---|---|
| under 32us | 1619 | 2.4% |
| 64us | 59803 | 89.9% |
| 128us | 4998 | 7.5% |
| 256us | 84 | 0.13% |
| 512us | 1 | |
| 1024us | 1 | |

Mean 102us, worst 1339us, no bus errors and nothing logged by either side.

So the stock kernel carries this workload: 99.8% of cycles complete inside
128us, and the distribution thinned as the run went on rather than drifting.
What an RT kernel would buy is the far tail - a single 1.3ms excursion,
which on a SCHED_FIFO thread with RT throttling disabled has a specific
cause worth knowing (a preemption-disabled section, an interrupt storm, SD
I/O). At one occurrence in 66506 the rate is barely measured, and 1.3ms is
three orders below anything 2.11BSD reacts to - its line clock ticks every
16.6ms.

Worth returning to only if a longer run shows those excursions recurring at
a rate, or reaching far enough to matter. `linux-image-6.12.93-bone-rt-r63`
is one `apt install` away, and the same measurement under both settles it.

## The device tree has to be rebuilt regardless

`bone_capemgr` was removed in **4.19**, and the cape overlay currently loads
through it, from the cape's own EEPROM. Any move off the 4.9 kernel has to
replace that with a U-Boot applied overlay (`uboot_overlay_addr4=`), and the
overlay source syntax changed with it: `.dtso`, targets written as
`&label { }` rather than `fragment@N`/`__overlay__`, no `part-number`,
`version` or `exclusive-use`, and different pin macros.

The AM335x device tree was also restructured onto `ti-sysc` in 4.20, moving
peripherals under `target-module@` nodes with relative `reg` values. Overlays
that name their targets by label generally survive that; anything naming a
path does not.
