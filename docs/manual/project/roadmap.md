---
title: What is coming
description: Which parts of this site are written, which are being written, and what the plan is for the rest.
---

This site replaces the separate UniBone and QBone project pages on retrocmp.com
with one manual covering both cards. It is being built in phases, and this page
says honestly where it has got to.

## Written

- **Start here** — what QUniLator is, choosing a card, getting one, installing the
  software, the acceptance test.
- **The card** — the card itself, fitting it to a backplane, bus drivers.
- **Background** — the PDP-11, UNIBUS and QBUS.
- **Configurations** — the catalogue and the format a catalogue is published in.

## Being written

**The operator's guide.** The largest gap. The retrocmp articles document the
`demo` menu application; what an operator uses now is the web interface, DIP-selected
configurations, the image library and the REST API. That is a rewrite rather than
a migration, covering:

the web interface · configurations and autostart · disk and tape images, and
sharing them over SMB, FTP and SFTP · the console, emulated and bridged ·
emulated memory and the address map · networking · booting a guest · running
XXDP diagnostics · updating QUniLator

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

The catalogue on this site is ahead of QUniLator. Two pieces are in
progress in the QUniLator repository:

- **[#81](https://github.com/QUniBone/QUniLator/issues/81)** — a configuration
  carries its own documentation, in named fields, travelling inside the
  `.qcfg.zip` as `readme.md`. This site already stores and renders exactly those
  fields.
- **[#64](https://github.com/QUniBone/QUniLator/issues/64)** — QUniLator
  subscribes to catalogues and imports from them, instead of an operator finding
  a URL and downloading a bundle by hand.

Until #64 lands, the catalogue entries here are downloaded by hand and fed to the
import dialog. The [format](../configurations/format.md) is published now so that
anyone wanting to run their own catalogue can build against it rather than wait.

## Helping

The manual is Markdown in git, and every page has an *Edit this page* link at the
bottom. Corrections are welcome, and so are photographs — particularly of UniBone
in a live machine, which the project is short of.

If you have **UNIBUS hardware**, reports are especially valuable: UniBone builds
and packages from the same source as QBone but has not been exercised on real
hardware in this codebase.
