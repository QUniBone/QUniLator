---
title: Catalogue format
description: How to publish a catalogue of configurations that any QUniLator board can subscribe to.
---

A board subscribes to a **list** of catalogues — the project's own, a user
group's, an operator's private one — so publishing your own is a first-class
thing to do, not a fork of this site.

A catalogue is a **static JSON file on any web server**. There is no software to
run.

> [!WARNING]
> **Not settled yet**
>
> The board-side implementation of catalogue subscription is
> [in progress](../project/roadmap.md). What follows is the shape this site
> publishes today at
> [`/catalog/v1/index.json`](https://qunilator.com/catalog/v1/index.json); it is
> the proposal the board will be built against, and it may still move before it
> is final.

## The index

```json
{
  "schema": "qunilator-catalog/1",
  "name": "The QUniLator project's own catalogue",
  "updated": "2026-08-05",
  "configurations": [
    {
      "id": "xxdp-rl02",
      "title": "XXDP 2.5 on an RL02",
      "summary": "DEC's diagnostic monitor on an emulated RL11 with four RL02 drives.",
      "bus": "qbus",
      "devices": ["MEM", "MRV11-D", "DL11", "RL11", "RL02"],
      "guest": "XXDP 2.5",
      "page": "https://qunilator.com/configurations/xxdp-rl02/",
      "download": {
        "url": "https://…/xxdp-rl02.qcfg.zip",
        "bytes": 11534336,
        "sha256": "…"
      },
      "doc": { }
    }
  ]
}
```

| Field | Why the board needs it |
|---|---|
| `bus` | A UNIBUS configuration names devices a QBUS board does not have. The board offers only what it can run. |
| `devices` | Shown before download, so an operator sees what the machine carries. |
| `bytes` | Media runs to hundreds of megabytes. The board reports progress against this rather than downloading blind. |
| `sha256` | Verified before import. A truncated bundle must fail loudly, not half-import. |
| `page` | Where a human reads the full documentation. |
| `doc` | The structured documentation below. |

## The documentation block

Every configuration carries its own documentation, in named fields rather than
free prose — so a catalogue can say what a machine is for without parsing
anybody's Markdown, and a form can validate it.

| Field | |
|---|---|
| `motivation` | What this machine is for. |
| `usage` | A list: how to log in, what to try first. |
| `bugs` | A list of known problems. May be empty. |
| `links` | `{label, url}` pairs — manuals, tickets, sources. |
| `maintainer` | `{name, contact}`. Who to ask. |
| `added` | `YYYY-MM-DD`. |

The fields carry links and lists, not full Markdown: enough for a usage list and
a links section, little enough to render without sanitising a document somebody
else wrote.

A configuration arriving **without** documentation, or short a field, is still
imported and marked *undocumented* — every configuration that exists today is
one, and refusing them would make this a wall rather than a habit. The board says
what is missing, and a catalogue may too.

## Serving it

Two requirements beyond putting the file somewhere:

**CORS.** A board on an isolated LAN has no route to your web server, but the
operator's browser usually does — in that case the browser fetches the index and
the bundle and posts what it got to the board. That only works if the catalogue
is served with `Access-Control-Allow-Origin: *`. This site does; so should yours.

**Stable URLs for bundles.** The board records what it has imported by `id` and
checksum. Re-publishing a bundle at the same URL with different contents makes
the board's record wrong — give the new one a new `id`, or a versioned URL.

## Where the bundles live

Not in git. A `.qcfg.zip` carries its media and runs to tens or hundreds of
megabytes, so bundles are published as release assets and the catalogue points at
them. What is committed is the entry: the metadata, the documentation, and the
checksum of the bundle it names.
