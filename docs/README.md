# Documentation

Everything the project writes down that is not source. The user-facing manual
is one part of it; the rest is the working record — what was planned, what a
board turned up, and how a release is made.

## The manual

| | |
|---|---|
| [`manual/`](manual) | The user manual, task-shaped and readable as plain Markdown in a checkout. |
| [`site/`](site) | The Astro generator that publishes the manual; [`site/STRUCTURE.md`](site/STRUCTURE.md) records how it is organised. |

## Planning

| | |
|---|---|
| [`planning/`](planning) | Requirements per area of work, one document each. [`planning/README.md`](planning/README.md) is the index and the status board. |
| [`plans/`](plans) | Implementation plans, written once an area's requirements settle. |

## Building and running a card

| | |
|---|---|
| [`distribution.md`](distribution.md) | The downloadable card image: what it carries, how it is built, and how a card updates through apt afterwards. |
| [`release-notes.md`](release-notes.md) | How a release note is written: where its text comes from, the shape it takes, and the voice it is written in. |
| [`debian-installation.md`](debian-installation.md) | Setting a BeagleBone up from scratch — the Debian base, its settings, and the cape overlay. |

## Findings

Investigations and bring-up records, each kept for what it establishes about
the hardware or a guest.

| | |
|---|---|
| [`unibone-bringup-issues.md`](unibone-bringup-issues.md) | What the first UniBone board turned up against a QBUS-developed image, and how each was fixed. |
| [`vax-host.md`](vax-host.md) | Where the VAX UNIBUS host stands: a VAX-11/780 core running as a device of the application, booting VMS from the emulated UDA50. |
| [`vax-boot-flakiness.md`](vax-boot-flakiness.md) | Why that VAX booted about half the time — an interrupt granted with a zero vector. |
| [`vcb01-status.md`](vcb01-status.md) | VCB01/QVSS emulation end to end: bus device, framebuffer, refresh worker, X renderer. |
| [`vcb01-2bsd-driver.md`](vcb01-2bsd-driver.md) | What giving 2.11BSD a program-drawable framebuffer on the VCB01 takes. |
| [`kw11p-zkwb-findings.md`](kw11p-zkwb-findings.md) | The KW11-P model against its ZKWB diagnostic. |
| [`rsx-delqa-netgen.md`](rsx-delqa-netgen.md) | Bringing RSX-11M+ DECnet up on the emulated DELQA, and the QNA-0 generation it needed. |

## Standing list

[`todo.md`](todo.md) — work worth doing that is not tied to a single plan.
