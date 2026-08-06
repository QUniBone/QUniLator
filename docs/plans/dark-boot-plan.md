# Coming up on a bus that is not ours — implementation plan

A UniBone is fitted into a machine that already exists. What that machine
carries is decided by its backplane, not by anything the board was told
earlier, and the board is often fitted precisely because the machine is
misbehaving. So the board must not put anything on the bus on the strength of
a statement made at some other time, on some other backplane.

Today it does, twice: a stored setting decides whether the emulated processors
exist at all, and a configuration applied at startup goes straight onto the bus
unattended. This plan removes the first and closes the second.

## 1. What exists today

**The processors are built or not built at startup.**
`device_configuration_c` constructs `CPU20`, `CPU34` and `CPUVAX` only inside
`if (with_emulated_CPU)` (`10.03_app_demo/2_src/device_configuration.cpp:148`),
and enables `CPU20` there and then (`:161`). The flag comes from
`websettings_emulated_cpu()` (`main_qbone_web.cpp:165`), which reads
`emulated_cpu` from `settings.json`, default `false`
(`10.05_web/2_src/websettings.cpp:65`).

The consequences follow from that one boolean:

- With it off, the processors are absent from `/api/devices`, and a saved
  configuration naming `CPU20` cannot find it — the fault that started this.
- Changing it stores a value and warns that a restart is needed
  (`websettings.cpp:304-317`), because the device set is built once. Even the
  one path that rebuilds it — handing the board back after a `<name>-cli`
  session, `main_qbone_web.cpp:256` — passes the `emulated_cpu` local captured
  at startup, so it returns the machine that was given up rather than one built
  from the new setting.
- The frontend offers it as System → Machine → "Processor"
  (`10.05_web/3_frontend/src/components/Machine.ts:30-49`), guarded by
  `emulated_cpu_available`, which is `#if defined(UNIBUS)`
  (`websettings.cpp:234-236`).

**What the flag really controls is bus arbitration.** `devices_startup()` sets
the initial mode from it (`10.03_app_demo/2_src/menu_devices.cpp:151-156`):
with an emulated CPU, `ARB_MODE_NONE`; without, `ARB_MODE_CLIENT`
(`10.01_base/2_src/arm/qunibus.cpp:473-481`). `CLIENT` means somebody else
arbitrates this bus — ask before using it. `NONE` means nothing arbitrates, so
the board's devices take the bus when they want it. Getting that wrong in the
`NONE` direction on a live machine is emulated devices doing DMA into a running
CPU's cycles.

**The commitment happens at enable, not at run.** `cpu_base_c::on_before_install()`
— called when `enabled` goes true — ends with `stop("CPU stopped", ...)`
(`10.02_devices/2_src/cpu.cpp:310`), and `stop()` calls
`set_arbitrator_active(false)` (`:383`). So the instant a configuration enables
a processor, the board declares that nothing arbitrates this bus, whether or not
an instruction is ever executed. `start()` sets `CLIENT` again (`:355`), the
emulated CPU now being the arbitrator itself.

**The startup apply is live, not dark.** `webpower.cpp` models a switched-off
machine: while `machine_dark` is true, applying a configuration only records
devices in the `carried` list (`webconfigs.cpp:1512` →
`webpower_set_in_machine()`, `webpower.cpp:301`), nothing is installed, no
register handler is plugged into the I/O page, and no CPU reaches
`on_before_install()`. `webpower_devices_on()` (`:233`) walks that list and
enables them for real.

But `machine_dark` initialises to `false` (`webpower.cpp:70`). So the startup
apply, `webconfigs_startup()` (`webconfigs.cpp:1706-1720`), takes the other
branch — `dev->enabled.set(...)` at `webconfigs.cpp:1514` — and the machine the
DIP switches name goes onto the bus during boot, with nobody present. The
board-claim resume path does the opposite deliberately: it comes back with
`webpower_devices_off()` and leaves the panel switch to the operator
(`main_qbone_web.cpp:265-272`).

**Nothing checks the configuration against the backplane.**
`address_conflict_locked()` (`webpower.cpp:315`) compares carried devices
against each other. It knows nothing about real cards, so a configuration
enabling an RL11 on a board fitted to a machine that has one puts two responders
on 774400 with no complaint.

**There is no warning channel on an apply.** `/api/settings` collects
`warnings[]` and the frontend surfaces them
(`10.05_web/3_frontend/src/api.ts:112-118`). `webconfigs.cpp` has nothing of the
kind: an apply returns success or errors.

## 2. What is wrong with the design, not the code

`emulated_cpu` is a declaration about the world outside the board — is a real
processor arbitrating this bus — stored on the board and never re-asked. It
travels with the card exactly as a configuration does. Fit that board into a
different backplane and the setting is as stale as anything else, and it fails
silently, because nothing ever revisits it.

The same staleness reaches every device, not just processors. A configuration
saved a year ago describes a machine that may no longer be the machine the board
is in. The processor is the loudest instance of that fault, not a separate one,
and a processor-specific setting was only ever covering one instance of it.

A probe cannot replace the declaration either. This board is fitted to machines
that have faults; silence from the bus does not prove nothing is there, and an
answer may be wrong. A probe is diagnostic text, never a gate.

What is left, once no stored statement and no measurement can be trusted, is
timing: do nothing to the bus until a person is present, and when a standing
instruction says otherwise, say so loudly enough that the record shows who asked.

## 3. The design

### 3.1 The processors always exist

Construct `CPU20`, `CPU34` and `CPUVAX` unconditionally on the UNIBUS build, all
**disabled**. They cost nothing while disabled: no registers on the bus, no
allocation in the constructors (`10.02_devices/2_src/cpuvax.cpp:45-80`), and a
disabled `qunibusdevice_c` installs no page handlers. `CPU20->enabled.set(true)`
at `device_configuration.cpp:161` goes: which processor a machine has is what a
configuration says, not what a build-time flag says.

Consequences: `/api/devices` lists the processors on every UNIBUS board, a
configuration naming `CPU20` applies, and none of it needs a service restart.

### 3.2 Arbitration follows the device set

An enabled emulated processor means this board is the machine: `NONE` while it is
halted, `CLIENT` while it runs. No emulated processor enabled means `CLIENT` —
the safe direction, since asking for a bus nobody arbitrates costs a refused
request, while not asking on a bus somebody does arbitrate corrupts a running
machine.

`start()` and `stop()` already write the two running-state transitions
(`cpu.cpp:355`, `:383`). What changes is that `stop()` must not declare `NONE`
unconditionally: it does so today for a processor that is enabled but idle, which
is wrong on a board that is a peripheral. The rule to encode is *enabled*, not
*running*.

### 3.3 `emulated_cpu` is deleted

With 3.1 and 3.2 there is nothing left for it to decide. Remove the setting, the
`emulated_cpu_available` field, the `PUT` handling (`websettings.cpp:304-317`),
the `--emulated-cpu` option (`main_qbone_web.cpp:109`) and the Processor control
in `Machine.ts`. A stored value in an existing `settings.json` is ignored; no
migration is needed, because the fact it carried is now carried by whether a
configuration enables a processor.

The startup memory claim that currently hangs off the flag
(`main_qbone_web.cpp:183-199`) — the board supplying everything below the I/O
page that no physical card answers, because an emulated processor has no machine
around it — moves to the point where a processor is enabled.

### 3.4 The board boots dark

`machine_dark` initialises to `true`. `webconfigs_startup()` loads the
configuration the DIP switches name into the dark machine: it is complete,
visible in the API and on the dashboard, and nothing of it has touched the bus.
The operator throws the switch — `POST /api/control {"action":"dc_on"}`, or the
`powercycle` that implies it (`webapi.cpp:740`, `:912-914`).

This is the whole safety property. Every other part of the plan is bookkeeping
around it.

### 3.5 Autostart is opt-in, per configuration, and announces itself

A board that must come up running its machine unattended is a real requirement,
and no placement of that instruction is safe: an `autostart` flag is a standing
instruction, and every standing instruction is stale by construction. It is
therefore not made safe, it is made **loud and attributable**:

- The flag lives in the configuration document, exports and imports with it, and
  defaults to off — so every configuration that exists today becomes safe on
  upgrade.
- When it fires, the board records a `WARNING` in the journal and raises a
  dashboard banner that **names what it put on the bus**: "autostarted
  `<config>` unattended: CPU20, RL11 at 774400, DL11 at 777560". Not
  "configuration autostarted" — the text has to be recognisable to somebody
  troubleshooting a machine that went strange after a board was fitted.
- The banner persists until dismissed, the way the update announcement does
  (`websettings.cpp:61`, `dismissed_version`), so it lives on the board rather
  than in one browser. The dismissal is the acknowledgement, and it is the part
  that stands up afterwards: the board can show that the warning was raised and
  that somebody cleared it.

A DIP setting is *not* treated as the request. A switch left where it was a year
ago is stale in the same way a configuration is.

### 3.6 A warning channel on apply

`/api/configs/<name>/apply` gains `warnings[]` alongside its errors, and the
frontend shows them the way it shows the settings warnings
(`api.ts:112-118`). Two warnings use it:

- applying a configuration that enables a processor, which is the interactive
  form of the commitment described in 1;
- the autostart notice of 3.5.

### 3.7 The probe, as advice only

Before power-on, with an operator present, the board may report what the bus
already answers — "the bus answers at 774400 and 777560" — beside the power
switch. It is text, it gates nothing, and it is never phrased as a verdict. On a
faulty machine it may be silent when cards are present or answer when they are
not, and the interface must not imply otherwise.

## 4. Order of work

1. **Warning channel** (3.6). Independent, small, and needed by everything after.
2. **Processors always constructed** (3.1) with arbitration following the device
   set (3.2). This alone fixes the reported fault — missing CPUs, unfindable
   `CPU20`, the restart — and is testable on a bench board with no machine around
   it.
3. **Delete `emulated_cpu`** (3.3), including the memory claim move.
4. **Boot dark** (3.4). Behavioural change for every board: a board that used to
   come up running its machine now comes up with the machine loaded and switched
   off.
5. **Autostart flag and its announcement** (3.5), which restores the previous
   behaviour for anybody who wants it, deliberately.
6. **Probe text** (3.7), last and optional.

Steps 4 and 5 belong to one release: shipping 4 without 5 takes away unattended
boot with nothing offered in its place.

## 5. What to check while building it

- `10.05_web/tools/config_test.cpp` constructs a device configuration and knows
  about the CPU gate; it moves with 3.1.
- `device_label.cpp` carries the processor labels — they now apply to a board
  whose processors are merely present, so the wording should not imply a
  machine that is running.
- The `cpu_base_c::on_before_install()` guard against a second processor
  (`cpu.cpp:299-303`) becomes reachable in ordinary use once all three
  processors always exist. It already refuses correctly; the API needs to
  surface that refusal rather than logging it.
- `10.05_web/3_frontend/src/store.ts:60-61` holds `emulated_cpu` and
  `emulated_cpu_available` in the settings model; both go with 3.3.

## 6. What was built

Steps 1 to 5 are in, on `feat/dark-boot`, verified on the board and in the
browser. Two things differ from the plan above:

- **Switching autostart on asks first.** The plan had the flag announcing
  itself only after the fact, which leaves the one moment a person is present
  unused. The checkbox now raises a confirmation naming the cards that will go
  onto the bus and saying what the board cannot check — that it reads no
  backplane, and that a configuration outliving the machine it was written for
  means two cards on one address or a second processor. Switching it off asks
  nothing.
- **The DIP-0 case carries autostart through the mirror.** "The machine as it
  last stood" is an edit of a named configuration, so that configuration's flag
  governs it; a machine no configuration names stays dark.

Step 3.7, the advisory bus probe, is **not built**. It was optional, and it
wants somewhere to live on the power-up screen first.
