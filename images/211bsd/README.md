# 2.11BSD for a QBone

`QBONE` is the 2.11BSD kernel configuration for a QBUS PDP-11 driven by a
QBone cape. It carries the QBUS DEQNA/DELQA driver, so the emulated DELQA at
174440 comes up as `qe0`, and it is built for the J-11 processors a QBone is
fitted to.

Where it differs from the `PIDP11` configuration 2.11BSD ships in the PiDP-11
distribution:

| item | value | why |
|---|---|---|
| `IDENT` | `QBONE` | names the configuration and the build directory |
| `NQE` | 1 | the QBUS DELQA the QBone emulates |
| `NDE` | 0 | leaves out the UNIBUS DELUA, which a QBone has no bus for |
| `PDP11` | 73 | selects the PSW access the J-11 uses |
| `LINEHZ` | 50 | mains-derived KW11 line clock |
| `TIMEZONE` | -60 | minutes west of GMT, so central Europe |

## Rebuilding the kernel

The build runs inside the guest, under simh with a QBUS CPU: a DELQA is a
QBUS card and simh disables XQ on an 11/70.

    set cpu 11/73 4096K fpp
    set rq enabled
    set rq0 RA81
    attach rq0 2.11BSD_qbone.dsk
    set xq enabled
    set xq type=delqa
    boot rq0

Boot to the single-user shell and mount `/usr`, which carries the kernel
sources and is not mounted for you:

    /sbin/mount /dev/ra0g /usr

Then, with this file installed as `/usr/src/sys/conf/QBONE`:

    cd /usr/src/sys/conf
    ./config QBONE

`config` is the shell script in that directory. A bare `config` finds
`/usr/sbin/config`, which reports that config has not been implemented under
2.11BSD.

`config` writes its own overlay layout, which puts `toy.o` in OV7 and
overflows it — the link then fails with `ld: too big for type 431` on
`unix.o`. Move it to OV8 before building:

    cd ../QBONE
    sed -e 's/subr_log.o toy.o vm_swp.o/subr_log.o vm_swp.o/' Makefile > /usr/tmp/m1
    sed -e '/^OV8=/s/vm_text.o/vm_text.o toy.o/' /usr/tmp/m1 > Makefile

    make
    cp /unix /unix.old ; cp /netnix /netnix.old
    cp unix /unix ; cp netnix /netnix

`/etc/netstart` starts the interface, and names `de0` as distributed:

    sed -e '/ifconfig de0/s/ifconfig/#ifconfig/' /etc/netstart > /usr/tmp/n1
    sed -e '/# ifconfig qe0/s/# ifconfig/ifconfig/' /usr/tmp/n1 > /etc/netstart

Halt with `/sbin/halt`, which is in `/sbin` — as are `mount` and the other
tools the single-user `PATH` does not cover.

## Notes for anyone scripting this

The guest shell is the V7 shell: an unquoted `^` is a pipe, and `grep` takes
basic regular expressions only — no alternation, no `-i`. `egrep -i` and
`cut` are absent.

The configuration file is tab-separated, so a `sed` address matching a run of
spaces between the name and its value matches nothing. Match on the trailing
comment text instead.

The console tty is canonical with a 256-byte line limit. A longer command line
draws BELs and arrives truncated, and the fragment may still run, so keep
generated command lines short.

Driving simh over its telnet console works from a raw socket in a single
process; simh exits when the console connection drops, so the whole session
has to live in one invocation.

## The image

`2.11BSD_qbone.dsk` is a 2.11BSD install with this kernel, built from the
distributed PiDP-11 image. It keeps the kernel it replaced as `/unix.pidp11`
and `/netnix.pidp11`, and carries the build tree in `/usr/src/sys/QBONE`, so
it can rebuild itself.

The image is not in this repository — it is a gigabyte of disk. See
`DISTRIBUTION.md` for where the sample images are published.

## QBONETS: the same machine with a TS11 instead of a TMSCP tape

`QBONETS` is `QBONE` with the TS11 driver in place of the TMSCP one, for a
machine whose tape is QUniLator's emulated TSV05 at 172520 rather than a TK50.

| item | value |
|---|---|
| `IDENT` | `QBONETS` |
| `NTS` | 1 — the TS11/TSV05 |
| `NTMSCP`, `NTMS` | 0 — the TMSCP tape goes, to make room |

The MSCP *disk* the system boots from is the `ra` driver and is untouched.

Two things this build turns on that a plain rebuild does not:

**Keep config's own Makefile.** The README above tells you to restore
`Makefile.old` after `config`, which is right for rebuilding an unchanged
configuration. It is wrong when a driver has been added: that file is the
object list from before, so the kernel links without the new driver and
nothing complains. The tell is `ts.o` never appearing in the make log.

**Move the driver into an overlay.** `config` puts `ts.o` in the non-overlaid
`BASE`, and 9 kB of driver there overflows the kernel:

    ld: too big for type 431
    *** Exit 2

`OV9` held `tmscp.o` and `tmscpdump.o`, which are 20-byte stubs once TMSCP is
configured out, so the TS11 goes there:

    sed -e 's/tm.o ts.o tty.o/tm.o tty.o/' Makefile > /usr/tmp/m2
    sed -e '/^OV9=/s/tmscp.o/ts.o tmscp.o/' /usr/tmp/m2 > Makefile

Then `/etc/dtab` decides what autoconfig probes — uncomment `ts`, comment
`tms` (the lines are tab separated, so match the trailing comment text):

    sed -e '/ts11 driver/s/^# //' -e '/tmscp driver/s/^tms/# tms/' /etc/dtab

A kernel that has it prints this at boot, where one built without the driver
says `ts ? csr 172520 vector 224 skipped:  No autoconfig routines.`:

    ts 0 csr 172520 vector 224 attached

`nm` on the finished kernel proves nothing either way — the build strips its
own symbols with symcompact, strcompact and symorder.

The image built from this configuration is `2.11BSD_qbonets.dsk`.
