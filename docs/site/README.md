# The documentation site generator

Builds [qunilator.com](https://qunilator.com/) from the manual in
[`../manual/`](../manual/) — one manual for both cards, replacing the separate
UniBone and QBone project pages on retrocmp.com.

Built with [Astro](https://astro.build/) and
[Starlight](https://starlight.astro.build/). The manual is plain Markdown,
written to read as well in git and on GitHub as it does on the web —
[`../manual/README.md`](../manual/README.md) records the conventions that keep it
that way.

## Working on it

From this directory:

    npm install
    npm run dev          # live preview on http://localhost:4321/
    npm run build        # production build into dist/

## Deploying

    npm run build                                              # qunilator.com
    SITE_URL=https://vaxbusters.org SITE_BASE=/qunilator npm run build

The base path is environment-driven; nothing in the pages changes between the
two. `.github/workflows/docs-site.yml` at the top of the repository builds and
publishes on a change under `docs/`.

## Where things are

| | |
|---|---|
| `../manual/` | The manual. Plain Markdown, and the photographs it uses. |
| `docs-root.mjs` | Where the manual is, and the addresses that point back at it. |
| `plugins/` | The Markdown transforms that give those pages their site rendering. |
| `src/content/configurations/` | Catalogue entries — one YAML file per published machine. |
| `src/pages/catalog/v1/index.json.ts` | The catalogue index a board subscribes to. |
| `src/components/` | The sidebar bus selector, the edit link and the document head. |
| `tools/check-links.mjs` | Fails the build on a link that does not resolve. |
| `tools/scrape-retrocmp.py` | Pulls the original 23 articles into `migration/` for conversion. |

**Read [STRUCTURE.md](STRUCTURE.md) before adding pages** — it records how the
manual is organised, how bus variance works, and the two rules that keep a deep
link honest.

## Licence

Documentation and code here are BSD 2-Clause, matching the rest of the project.
Photographs are Jörg Hoppe's, used with permission.
