# The QUniLator manual

One manual for both cards, published at **<https://qunilator.com/>**.

The pages are plain Markdown and read here as they read on the web. Two
conventions carry the site's formatting through GitHub without breaking it:

- A passage that applies to one bus only is a blockquote led by
  `**UNIBUS · UniBone**` or `**QBUS · QBone**`.
- A callout is a [GitHub alert](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax#alerts),
  optionally with a bolded title as its first body line.

Pages link to each other by file, so every link works in a checkout too. What
turns them into a site — the theme, the navigation, the configuration catalogue —
is in [`../site/`](../site/); [STRUCTURE.md](../site/STRUCTURE.md) describes it.

## Contents

**Start here** — read in order.

1. [What QUniLator is](start/what-it-is.md)
2. [Choose your card](start/choose-your-card.md)
3. [Getting a card](start/get-a-card.md)
4. [Installing the software](start/install.md)
5. [Acceptance test](start/acceptance-test.md)

**The card**

- [What is on the card](hardware/the-board.md)
- [Fitting it to a backplane](hardware/fitting-the-card.md)
- [Bus drivers](hardware/bus-drivers.md)

**Configurations**

- [Catalogue](https://qunilator.com/configurations/) — generated from the entries
  in `../site/src/content/configurations/`, so it has no page here.
- [Catalogue format](configurations/format.md)

**Background**

- [The PDP-11 and its buses](background/pdp-11-and-the-buses.md)

**Project**

- [Glossary](project/glossary.md)
- [FAQ](project/faq.md)
- [What is coming](project/roadmap.md)
- [Credits and licence](project/credits.md)

The site's navigation is ordered by the sidebar in `astro.config.mjs`; this list
mirrors it for anyone reading in git.

## Licence

BSD 2-Clause, matching the rest of the project. Photographs under `assets/` are
Jörg Hoppe's, used with permission.
