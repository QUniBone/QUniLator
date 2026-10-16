# Site structure

The site merges the separate UniBone and QBone project pages on retrocmp.com into
one manual. This file records how it is organised and why, so the shape survives
contact with new contributors.

## The organising principle

**Task-shaped, not hardware-shaped.** The old sites split by card, which left
QBone as a five-article stub telling readers to go and read UniBone's eighteen.
The cards differ in the bus they drive and very little else, so this is one
manual and bus variance is marked inline.

## Where the manual lives

The pages are in **`docs/manual/`**, beside this generator and beside the
project's other documentation, so the directory reads as a manual to anyone who
arrives at it in git or on GitHub. `docs-root.mjs` names the location; the
content collection's base, the paths Starlight's own Markdown transforms are
allowed to touch, and the *Edit this page* target all read it from there.

It resolves the location from its own file rather than the working directory —
during prerendering Astro runs from `dist/.prerender/`, so a path built from
`process.cwd()` points nowhere.

## Pages read as Markdown

A page is plain Markdown and is meant to read as well in a checkout, on GitHub
and in an editor's preview as it does on the web. Three conventions carry the
site's formatting through a plain Markdown renderer, each turned into its site
form by a plugin in `plugins/`:

**Bus-specific passages** are a blockquote whose first paragraph is the bolded
label:

```markdown
> **QBUS · QBone**
>
> QBUS runs 16, 18 or 22 bits, and the processor decides which.
```

`UNIBUS · UniBone` is the other label. `remark-bus-notes.mjs` renders these as
`<aside class="bus-note">`; anything else stays an ordinary quotation.

**Callouts** are [GitHub alerts](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax#alerts),
with an optional bolded first body line as the title:

```markdown
> [!WARNING]
> **Watch for shorts**
>
> The BeagleBone is a little fat.
```

`remark-github-alerts.mjs` converts these to Starlight asides —
`NOTE` and `IMPORTANT` to *note*, `TIP` to *tip*, `WARNING` to *caution*,
`CAUTION` to *danger*.

**Links between pages name the file**, so they resolve in a checkout:
`../start/install.md#reading-the-leds`. `remark-doc-links.mjs` turns those into
the paths the site serves and applies the base path. Pages only the site can
serve — the catalogue, generated from YAML — are linked at
`https://qunilator.com/…`, which the same plugin folds back to a local path so a
subdirectory deployment stays linked to itself.

Images are written as ordinary Markdown, relative to the page. Astro resolves and
optimises them from there, so `![…](../assets/photos/board-fit.jpg)` is both a
working link in git and a responsive `<picture>` on the site. On the site every
image opens at full size when clicked: `src/components/MarkdownContent.astro`
wraps each one in a link to the rendition Astro built and shows it in a
lightbox, so a screenshot that is scaled into the text column is still readable.

`tools/check-links.mjs` fails the build on a link that resolves to nothing, and
on a `.md` link that reached the built HTML unconverted.

## Sections

| Path under `docs/manual/` | What belongs there |
|---|---|
| `index.md` | Landing. What it is, the two cards, where to start. |
| `README.md` | The manual's contents page, for anyone reading it in git. |
| `start/` | Ordered. Read top to bottom: what it is, choosing a card, getting one, installing, acceptance test. |
| `guide/` | *Not yet written.* The operator's manual — web interface, configurations, images, console, memory, networking, booting a guest, diagnostics, updating. |
| `devices/` | Reference, one page per emulated card, with parameter tables written from the source. `index.md` is the overview and the parameters every device has. |
| `hardware/` | The card itself: the board, fitting it, bus drivers. Later: panels, BeagleBone notes. |
| `configurations/` | The format a catalogue is published in. The catalogue itself is generated here, from `src/content/configurations/`. |
| `background/` | The PDP-11, UNIBUS and QBUS. Material that has not changed since 1975. |
| `project/` | FAQ, roadmap, credits. Later: downloads, changelog. |
| `assets/photos/` | Every photograph, all Jörg Hoppe's, used with permission. |

`start/`, `guide/` and `background/` are read in order. `devices/`, `hardware/`
and `configurations/` are reference — arrived at by search or link.

Adding a page means two edits: the page itself, and its sidebar entry in
`astro.config.mjs`. The manual's `README.md` mirrors that sidebar for git readers
and wants the same addition.

## Device pages

A page in `devices/` is prose about a card — what it is, what the machine sees,
what has been verified — around a parameter reference that nobody writes by hand.
Under a heading of the author's, a marked block names the C++ class it documents:

```markdown
## Parameters

<!-- device-parameters: cpuvax_c -->
<!-- /device-parameters -->
```

A page can carry several blocks — a controller and its drives — each under its
own heading. `tools/device-params.mjs` fills each block from the class
declaration and its constructor: every parameter with its short name, type, unit,
default and the one-line description the web interface shows, split into
settings and running status, under headings one level below the author's. The
longer explanation of a parameter is a `///` comment directly above its
declaration in the source; declarations that follow it without a blank line
share it. A `/// @default <text>` line fills the Default column where the
starting value is not a literal.

The source is read once as the UNIBUS build compiles it and once as the QBUS
build does, deciding `#if` conditions on the bus macros and on numbers the file
defines. Where the builds differ, the row says how: a default or read-only flag
per bus, or a parameter one build carries alone. An array of parameters — one
per serial line — is one row, `tcp_port0 … tcp_port7`, with the varying index
written *n* in its description.

Base classes are walked up to a class that has a block of its own, which is
linked by the heading that block sits under. The overview page holds the blocks
for what every device, every card on the bus and every drive carries, so those
are written once. Each class is documented in one block only; the generator
refuses a second.

The build runs the generator first (`prebuild`), so the site always shows what
the source says. The pages are committed with the blocks filled in, so they read
complete in git too; CI runs `node tools/device-params.mjs --check` and fails
when a source change has left a committed page behind. Run the generator after
changing a device's parameters, and commit the page with the source.

## Bus variance

Every page may declare which bus it applies to:

```yaml
---
title: Fitting it to a backplane
bus: [unibus, qbus]   # the default; omit unless narrowing
---
```

Inside a page, a passage that applies to one bus only is a labelled blockquote,
as above.

The **I have a** control in the sidebar sets `data-bus` on the document element,
persisted in `localStorage`. Two rules govern what that may do:

- **The sidebar may hide** entries for the other bus. That is navigation.
- **A page may not.** A bus note is always rendered and always labelled, whatever
  is selected — it only dims. A deep link must never hide content, site search
  must always find it, and a QBone operator is better off knowing UNIBUS differs
  here than seeing a gap.

To mark a **sidebar entry** as bus-specific, give it the attribute in
`astro.config.mjs`; the CSS in `src/styles/bus.css` does the rest:

```js
{ label: 'Emulated processors', slug: 'guide/cpu', attrs: { 'data-bus': 'unibus' } }
```

## The configuration catalogue

`src/content/configurations/*.yaml` is the single source for two outputs:

- **Human pages** at `/configurations/<id>/`, rendered by `src/pages/configurations/[id].astro`.
- **The machine index** at `/catalog/v1/index.json`, which a board subscribes to.

They cannot drift apart because they are the same records. The schema in
`src/content.config.ts` mirrors the documentation fields defined by QUniLator
issue #81, so an entry here and the `readme.md` inside a `.qcfg.zip` carry the
same information.

These entries live with the generator: they describe published machines rather
than the software, and each is a small YAML file that the site is the only reader
of.

Bundles are **not** committed — a `.qcfg.zip` carries its media and runs to
hundreds of megabytes. They are published as release assets; what lives in git is
the metadata, the documentation and the bundle's checksum.

`/catalog/*` is served with permissive CORS, because a board on an isolated LAN
cannot reach this site but the operator's browser can. Where that is configured
depends on the host: `public/_headers` covers Netlify and Cloudflare Pages, while
the current deployment on Caddy takes it from `header_paths` on the
`qunilator.com` static site in the `netzhansa-infra` repository. Both are in
place; a change to one wants the same change to the other.

## Deploying to a subdirectory

The site is served at the root of `qunilator.com`, which is what a plain build
produces. The base path is environment-driven all the same, so a subdirectory
deployment needs no edits to the pages:

```sh
npm run build                                    # https://qunilator.com/
SITE_URL=https://example.org SITE_BASE=/docs npm run build
```

`remark-doc-links.mjs` applies the base to every link a page carries. Two things
it does not cover, so watch for them:

- **Frontmatter links** (the hero actions in `index.md`) — write those relative.
- **Sidebar `link:` entries** — Starlight prefixes the base itself. Do not prefix
  them again.

`.astro` pages use `import.meta.env.BASE_URL`.

## Migration

`tools/scrape-retrocmp.py` pulls the 23 original articles into `migration/` —
raw HTML, converted Markdown, and every image. The Markdown is **staging, not
content**: it is a starting point for hand-editing into `docs/manual/`.

Pages about the **software** are rewritten rather than migrated — the originals
document the `demo` menu application, which the web interface has replaced. Pages
about **hardware, buses and machines** are close to the originals, because that
material has not changed.

Redirects from the retrocmp URLs are still to be arranged; the old article URLs
are cited in mailing lists and forum threads and should not rot.
