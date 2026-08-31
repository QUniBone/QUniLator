---
title: What is coming
description: Which parts of this site are written, which are being written, and what the plan is for the rest.
---

This site replaces the separate UniBone and QBone project pages on retrocmp.com
with one manual covering both cards. It is being built in phases, and this page
says honestly where it has got to.

## Written

- **Start here** — what QUniLator is, choosing a card, getting one, installing the
  software, the acceptance test, and what changes for an operator arriving from
  QUniBone Classic.
- **The card** — the card itself, fitting it to a backplane, bus drivers.
- **Background** — the PDP-11, UNIBUS and QBUS.
- **Configurations** — the catalogue and the format a catalogue is published in.
- **Operating** — the web interface, screen by screen.
- **Walkthroughs** — three machines taken from a bare card to a running system,
  each verified on hardware and carrying the version it was checked against.
- **Tools** — the MCP server, which gives a model the run of a machine.

## Being written

**The operator's guide.** The tour of the web interface and the three
walkthroughs are written. What each walkthrough had to explain in passing wants
a page of its own, and those come next:

configurations and autostart · disk and tape images, and sharing them over SMB,
FTP and SFTP · the console, emulated and bridged · emulated memory and the
address map · networking · running XXDP diagnostics · updating QUniLator

More walkthroughs follow the same rule as the first three: written from a run on
real hardware, and re-run from the written text before they are published.

**The debug workbench** — memory and disassembly views — is in the source but
not yet in a release, so it has no page here. It gets one when it ships.

## Planned

**A device reference**, one page per emulated card — what it is, its parameters,
what the guest sees, and which DEC diagnostic it passes. Generated from the
source rather than hand-maintained, so it cannot drift from what the software
actually does.

**A compatibility matrix** — guest operating system against device, with status
and a link to the notes. The project knows a great deal of this and it is
currently scattered across commit messages and design documents.

**A developer manual** — the theory of operation, building from source, writing a
device, the emulated processor cores, the REST and WebSocket API reference, and
debugging.

**Redirects from retrocmp.com.** The old article URLs are cited in mailing lists
and forum threads and should not rot. Until they are in place, the original
[UniBone](https://retrocmp.com/projects/unibone) and
[QBone](https://retrocmp.com/projects/qbone) pages remain the reference for
anything not yet migrated here.

## The configuration catalogue

QUniLator subscribes to catalogues and imports from them — the Catalogue screen
in the web interface, reading the [format](../configurations/format.md) this
site publishes at `/catalog/v1/index.json`. Still open:

- **[#81](https://github.com/QUniBone/QUniLator/issues/81)** — the structured
  documentation travelling *inside* the `.qcfg.zip` as `readme.md`, so a bundle
  handed around outside any catalogue still documents itself. This site and the
  catalogue index already carry exactly those fields.

## Helping

The manual is Markdown in git, and every page has an *Edit this page* link at the
bottom. Corrections are welcome, and so are photographs — particularly of UniBone
in a live machine, which the project is short of.

If you have **UNIBUS hardware**, reports are especially valuable: UniBone builds
and packages from the same source as QBone but has not been exercised on real
hardware in this codebase.
