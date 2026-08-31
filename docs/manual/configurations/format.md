---
title: Catalogue format
description: How to publish a catalogue of configurations that any QUniLator can subscribe to.
---

A QUniLator subscribes to a **list** of catalogues — the project's own, a user
group's, an operator's private one — so publishing your own is a first-class
thing to do, not a fork of this site.

A catalogue is a **static JSON file on any web server**. There is no software to
run.

This is the shape this site publishes at
[`/catalog/v1/index.json`](https://qunilator.com/catalog/v1/index.json) and the
shape QUniLator's Catalogue screen reads. The `schema` field is what lets it
grow: a reader refuses an index whose schema it does not know.

## The index

```json
{
  "schema": "qunilator-catalog/1",
  "name": "The QUniLator project's own catalogue",
  "updated": "2026-08-05",
  "configurations": [
    {
      "id": "211bsd",
      "title": "2.11BSD on the RA81",
      "summary": "Multi-user Unix on an emulated UDA50 MSCP disk, with DELQA Ethernet.",
      "bus": "qbus",
      "devices": ["UDA50", "RA81", "DELQA"],
      "guest": "2.11BSD",
      "page": "https://qunilator.com/configurations/211bsd/",
      "download": {
        "url": "https://…/211bsd.qcfg.zip",
        "bytes": 37139129,
        "sha256": "…"
      },
      "images": [{ "path": "du/2.11BSD_qbone.dsk", "bytes": 1000090112 }],
      "doc": { }
    }
  ]
}
```

| Field | Why QUniLator needs it |
|---|---|
| `bus` | A UNIBUS configuration names devices a QBone does not have. QUniLator offers only what it can run. |
| `devices` | Shown before download, so an operator sees what the machine carries. |
| `bytes` | Media runs to hundreds of megabytes. QUniLator reports progress against this rather than downloading blind. |
| `sha256` | Verified before import. A truncated bundle must fail loudly, not half-import. |
| `page` | Where a human reads the full documentation. |
| `images` | The bundle's disk images as images-root subpaths with sizes — so QUniLator can say which are already on the board and how much space an import still needs, before downloading anything. May be omitted. |
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
one, and refusing them would make this a wall rather than a habit. QUniLator says
what is missing, and a catalogue may too.

## Serving it

Two requirements beyond putting the file somewhere:

**A route from the board.** QUniLator fetches the index and the bundle itself,
so the board — not just the operator's browser — needs to reach your server.
Plain https on any host works; a bundle `url` may also be relative to the index
(`..` is not resolved — write such a URL absolute).

**Stable URLs for bundles.** QUniLator records what it has imported by `id` and
checksum. Re-publishing a bundle at the same URL with different contents makes
QUniLator's record wrong — give the new one a new `id`, or a versioned URL.

## Where the bundles live

Not in git. A `.qcfg.zip` carries its media and runs to tens or hundreds of
megabytes, so bundles are published as release assets and the catalogue points at
them. What is committed is the entry: the metadata, the documentation, and the
checksum of the bundle it names.
