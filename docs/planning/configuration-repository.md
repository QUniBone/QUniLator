# Configuration repository

**Status:** Gathering.

An operator shares a whole machine — a configuration with the media its drives
name — in one action from the web interface, and any QUniLator that subscribes
to the repository can import it.

## Current state

A configuration is a JSON document: `title`, `dip_value`, dashboard `layout`,
and `devices` with their parameters. `GET /api/configs/<name>?export=json`
returns it as a file; `?export=script` returns the same device set as commands
for the interactive menu.

The **bundle is assembled in the browser**. `exportConfigBundle()` fetches the
document, walks the drives' `image` parameters, pulls each file from
`GET /api/images/<subpath>`, and saves `<name>.qcfg.zip` holding the document
and an `images/` tree. Import runs the same path in reverse: the browser
unpacks the archive, writes the images QUniLator does not already hold, and
posts the document to `POST /api/configs/<name>/import`.

That import is already careful in the ways that matter. The name must be free,
an occupied one answering `409`. The document is validated against the known
devices, their parameters and the backplane slots before anything is written.
The DIP binding travels but does not displace one another configuration
claims, and the answer says so. `autostart` is dropped. The machine stays dark:
an import writes a configuration and applies nothing.

Images are held in a library of their own, reachable over SMB, FTP and SFTP,
and a file's owner write bit is honoured as the medium's write ring.

**Distribution stands at a published format and nothing that reads it.** The
catalogue schema `qunilator-catalog/1` is documented in
[the manual](../manual/configurations/format.md), and `docs/site` generates
`/catalog/v1/index.json` from one YAML entry per configuration. QUniLator
itself contains no catalogue code. The single published entry is a placeholder:
its `sha256` is sixty-four zeros and the release asset it names does not exist.

Two issues carry parts of this: **#81**, a configuration carrying its own
documentation, and **#64**, browsing catalogues from the interface.

## Requirements

### Sharing

- **One action.** From a configuration in the web interface: sign in, give a
  title and a one-line summary, choose who sees it, done. No web host to
  arrange, no file to place, no pull request.
- **Documentation is optional.** A bundle published without the doc fields
  imports normally and lists as *undocumented*, and can gain them later.
- **QUniLator uploads what it holds** when it can reach the repository. The
  browser relays when it cannot, which is the isolated-LAN case.
- **An update re-sends only what changed.**

### Subscribing and importing

- QUniLator holds a **list of catalogue URLs**, with the project's repository
  as the default entry, and refreshes them on demand.
- The public catalogue **reads anonymously** — browsing and importing need no
  account.
- A download is **verified against its checksum** before import, with progress
  reported against a known size.
- Import keeps its present contract: it writes a configuration and leaves the
  machine dark.

### Identity and access

- **Signing in makes a publisher.** A new account publishes to *unlisted* and
  *granted* visibility immediately, so sharing with named people is never
  gated.
- **Public listing carries a flag a maintainer sets**, once per person. From
  then on that account publishes publicly by itself.
- An owner **grants a bundle to people or to a group**.
- A user **creates groups** and adds members by email address. A grant made to
  an address that has no account yet matches when that person signs in.

### Adapting a bundle to the operator's hardware

- A bundle **declares what it assumes**: bus, address width, the memory it
  wants, and whether it needs an emulated processor.
- Import shows a **fit report** against what the machine answers, from
  `POST /api/memory/probe` and `GET /api/memory/map`.
- A bundle **marks its site-local parameters** — `DL11.serialport`, `MEM`'s
  address range, anything naming a host resource — and import asks for them,
  seeded with the publisher's values.

### Operations

- Storage is **per object, keyed by sha256**, shared across every bundle that
  names the same media.
- The standing upkeep is approving a first public publish and unlisting a
  bundle on request.

## Decisions

- **QUniLator speaks catalogue URLs and nothing else.** Identity, groups and
  grants live in the repository service and resolve into which entries a given
  URL shows: the public catalogue anonymously, a private one at a URL carrying
  a token. The emulator needs no account model.
- **Media is content-addressed.** Publishing sends the hashes first and uploads
  only the objects the repository lacks, so common media — the XXDP pack, a
  stock 2.11BSD image, the M9312 ROMs — is stored and transferred once. This
  is what makes a first publish fast and an update nearly free.
- **A bundle carries a manifest** — id, version, the doc fields, the declared
  requirements, the site-local parameters, and `{path, sha256}` per image.
- **The single-file `.qcfg.zip` remains** as the offline export, for
  sneakernet and for a machine with no route out.
- **The service is composed of managed parts**: Supabase for identity,
  metadata and row-level policies, Cloudflare R2 for the objects behind
  presigned uploads. Access rules are policies over a view, so the catalogue
  query and the permission check are one thing.
- **Visibility is public, unlisted or granted.**
- **The word is bundle**, as the [glossary](../manual/project/glossary.md) has
  it. What a catalogue lists is a configuration; what you download is a bundle.

## Open questions

- A large bundle in the browser relay: streaming the archive, or uploading each
  object as it is fetched so nothing large is held at once.
- Whether QUniLator publishes under the operator's signed-in session or holds a
  repository token of its own, and how such a token is installed and revoked.
- Sign-in providers — GitHub alone, or an email link beside it for people who
  have no GitHub account.
- Where the function that mints presigned uploads runs, and how its secret is
  held.
- Whether a version is a row of its own or the hash of its manifest.
- How the fit report reads on a machine that is switched off, where the address
  map is unknown.
- What a takedown does to objects that other bundles still reference.
- What becomes of the bundles of an account that goes quiet.
- Whether the site's YAML entries remain a hand-curated front page above the
  service's listing, or move into it.

## Implementation order

1. **The manifest**, inside the bundle — the enabling change for everything
   below, and the substance of #81.
2. **The service** — identity, tables, policies, presigned upload, and
   `GET /catalog/v1/index.json` with and without a token.
3. **Share…** in the web interface, with the QUniLator upload path and the
   browser relay.
4. **The catalogue list in QUniLator** — `catalogs` in settings, refresh,
   browse and import with checksum verification, which is #64.
5. **Groups**, then the fit report and site-local parameters.

Nothing here reaches the emulator: the work is the web service, the frontend,
and a repository that runs outside the appliance.
