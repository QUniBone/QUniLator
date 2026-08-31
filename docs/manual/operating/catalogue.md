---
title: The catalogue
description: Machines other people have published — browsed from the interface, fetched and imported by the board itself.
sidebar:
  order: 5
---

A **catalogue** is a list of ready-made machines on a web server — for each
one a configuration and every disk image its drives name, bundled as the same
`.qcfg.zip` archive the configuration export writes. The Catalogue screen
shows what the catalogues this QUniLator subscribes to have on offer, and
imports the one you pick: the board downloads the bundle itself, verifies it
against the catalogue's checksum, unpacks the images into the image library,
and imports the configuration. When it is done, the machine is on the
Configurations screen like any other.

Each entry says which bus it needs, how large the download is, and what of it
is already here: a machine whose configuration name is already on this
QUniLator is marked **imported**, one needing the other bus is marked and
cannot be fetched, and an entry whose images are already in the library says
so — those images are kept as they are, never overwritten, so re-importing a
machine does not touch media a drive may hold.

**Import** asks for the name the configuration should have here — it must be
free — and then the board works alone: the progress bar follows the download
and the unpacking, and the page can be left and reopened without losing it.
The import comes in dark, like any import: the DIP binding and autostart do
not travel, so the machine starts only when you start it.

## Subscriptions

The catalogues themselves are listed at the bottom of the screen — an ordered
list of index URLs, and a fresh QUniLator carries the project's own. Add a
URL to subscribe; anyone can publish a catalogue, since it is nothing but a
static JSON file next to the bundles on any web server. A catalogue that
stops answering keeps showing what it last offered, marked unreachable, so
its listing goes stale rather than blank.

**Refresh catalogues** asks every subscribed index again. The board fetches
the indexes and the bundles itself, so it needs a route to the catalogue's
server; the browser's own internet access does not help it.

## Publishing a catalogue

The index format — the `qunilator-catalog/1` JSON schema naming each bundle
with its size, sha256 and image list — has [its own page](../configurations/format.md),
and `tools/build-catalog-zip.py` in the repository builds a bundle from an
exported configuration document and prints its index entry. Publish the index
and the bundles on any web server, and hand the index URL around.
