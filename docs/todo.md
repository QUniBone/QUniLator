# TODO

Work worth doing that is not tied to a single feature plan.

## Surface failures instead of swallowing them

A device that refuses to come up reports success. Enabling the DL11 through the
API answers `200` with `"value": false`:

    PUT /api/devices/DL11/params/enabled  {"value":"1"}
    -> 200 {"name":"enabled","value":false,...}

The device stayed off because its serial port was already held, and the only
trace was `Another process has locked the comport` in the journal. A caller
reading the response sees a success-shaped body and has to notice that the
value it carries is not the value it asked for.

The chain drops the error at every step:

- `rs232.cpp:251` reports a failed open with `perror()` and returns. Nothing
  travels back to the caller, and `perror` writes to stderr, which under
  systemd lands in the journal unlabelled instead of going through the logger
  with the device's name on it.
- `parameter_c::set()` therefore commits nothing and raises nothing.
- `device_param_set()` in `webapi.cpp` answers `200` with whatever the
  parameter now holds. It has a `bad_parameter` path that answers `422` with
  the device's message — the failure just never gets there.

What to do:

- Make an attach or open failure travel back as `bad_parameter`, so the
  existing `422` path carries the reason to the caller.
- As a backstop independent of any one device, have `device_param_set()`
  compare the committed value against the requested one and refuse to answer
  `200` when they differ. That catches the whole class rather than the
  instances found so far.
- Route device-level errors through the logger rather than `perror`.
- `POST /api/configs/<name>/apply` collects rejections into `errors`, but a
  device that silently fails to enable is not a rejection, so it answers
  `{"ok": true, "errors": []}` with the machine half configured. It has to
  verify what it applied.

A saved configuration can record a device that never actually enabled, which is
how `211bsd.json` came to list a DL11 that cannot run on this board.

## Name lookups that a static link had been breaking

The emulator was linked with `-static`, which made every `getaddrinfo()` call
fault: glibc loads its name service backends with `dlopen()` even from a static
binary, and the modules it finds belong to the glibc the board runs rather than
the one the binary carries. On the bone - glibc 2.41, against a binary built on
2.36 - a lookup died with SIGFPE whether the name resolved or not.

The build is dynamic now and the builder matches the appliance distribution, so
this is fixed. Two things worth knowing:

- **civetweb was affected too.** It calls `getaddrinfo()` in `mg_inet_pton()`
  and `getpwnam()` in `mg_start2()`, both of which drew the linker's warning on
  every static build. The web server binds numerically, so nothing had reached
  the faulting path yet.
- Anything that resolves a name should be exercised on the board rather than
  only on a workstation, since this class of fault appears nowhere else.

## Segmentation fault on service shutdown

`systemctl stop qbone` killed the process with SIGSEGV rather than letting it
exit, observed 2026-07-22:

    qbone.service: Main process exited, code=killed, status=11/SEGV

The unit catches SIGTERM to shut the device set down in order, and the last log
line before the fault came from the DELQA worker, so a device thread is
plausibly still touching state the shutdown has already torn down. The unit's
`TimeoutStopSec=15` and SIGKILL backstop mean a reboot still proceeds, which is
why this has gone unnoticed.

Worth a run under a debugger with the device set that was live at the time
(`uda`, `uda0`, `DL11`, `KW11`, `delqa`) to find which thread faults.

## Probe the bus for conflicts before installing a device

Nothing stops two things answering one address. Enabling a device whose
registers sit where real hardware already lives drives the bus against a card
that is also driving it, and the first symptom is unrelated corruption.

Before installing a device, DATI-probe the addresses it would claim and refuse
to enable when something answers, naming the address that collided.

Notes for whoever picks this up:

- The probe only works *before* the device is installed. Once its registers are
  in the PRU's table, QBone answers itself and every address looks occupied.
- The same check applies to emulated memory ranges. `emulate_memory()` already
  sizes physical memory with `qunibus->test_sizer()`, so the machinery exists;
  what is missing is a check on a specific range before claiming it.
- A device at a floating address that reads as non-responding is
  indistinguishable from free space, so the probe can only refuse on a positive
  answer, never confirm that a range is safe.
- This is the same check the VCB01 framebuffer needs before it claims its
  256 KB bank; see `docs/plans/vcb01-plan.md`.
