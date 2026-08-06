// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import { unified } from '@astrojs/markdown-remark';
import { CANONICAL_SITE, DOCS_ROOT } from './docs-root.mjs';
import { remarkBusNotes } from './plugins/remark-bus-notes.mjs';
import { remarkDocLinks } from './plugins/remark-doc-links.mjs';
import { remarkGithubAlerts } from './plugins/remark-github-alerts.mjs';

/**
 * Where the build is going.
 *
 *   npm run build                     -> https://qunilator.com/
 *   SITE_BASE=/qunilator \
 *   SITE_URL=https://vaxbusters.org \
 *   npm run build                     -> https://vaxbusters.org/qunilator/
 *
 * Everything below derives from these two, so a subdirectory deployment needs no
 * edits to the content.
 */
const SITE = process.env.SITE_URL ?? 'https://qunilator.com';
const BASE = (process.env.SITE_BASE ?? '').replace(/\/+$/, '');

// https://astro.build/config
export default defineConfig({
	site: SITE,
	base: BASE || undefined,
	trailingSlash: 'always',
	// Addresses the site has served under a name it no longer uses. Astro prefixes
	// the base to neither half, and the built file lands where the key says, so the
	// key stays root-relative and the target carries the base itself.
	redirects: {
		'/hardware/the-board/': `${BASE}/hardware/the-card/`,
	},
	markdown: {
		// These run before Starlight's own transforms, so an alert has become a
		// `:::` directive by the time Starlight looks for one.
		processor: unified({
			remarkPlugins: [
				remarkBusNotes(),
				remarkGithubAlerts(),
				remarkDocLinks({ docsRoot: DOCS_ROOT, base: BASE, canonical: CANONICAL_SITE }),
			],
		}),
	},
	integrations: [
		starlight({
			title: 'QUniLator',
			description:
				'QUniLator is the software that turns a UniBone or QBone card into emulated DEC hardware, on a real UNIBUS or QBUS backplane.',
			social: [
				{ icon: 'github', label: 'GitHub', href: 'https://github.com/QUniBone/QUniLator' },
			],
			// Starlight's own transforms — asides and heading anchor links — run only
			// on `src/content/docs/`, and the manual is a directory of its own.
			markdown: { processedDirs: [DOCS_ROOT] },
			customCss: ['./src/styles/bus.css'],
			components: {
				EditLink: './src/components/EditLink.astro',
				Head: './src/components/Head.astro',
				Sidebar: './src/components/Sidebar.astro',
			},
			sidebar: [
				{
					label: 'Start here',
					items: [
						{ label: 'What QUniLator is', slug: 'start/what-it-is' },
						{ label: 'Choose your card', slug: 'start/choose-your-card' },
						{ label: 'Getting a card', slug: 'start/get-a-card' },
						{ label: 'Installing the software', slug: 'start/install' },
						{ label: 'Acceptance test', slug: 'start/acceptance-test' },
						{ label: 'Coming from QUniBone Classic', slug: 'start/from-qunibone' },
					],
				},
				{
					label: 'The card',
					items: [
						{ label: 'What is on the card', slug: 'hardware/the-card' },
						{ label: 'Fitting it to a backplane', slug: 'hardware/fitting-the-card' },
						{ label: 'Bus drivers', slug: 'hardware/bus-drivers' },
					],
				},
				{
					label: 'Operating',
					items: [
						{ label: 'The web interface', slug: 'operating/web-interface' },
						{ label: 'Dashboard', slug: 'operating/dashboard' },
						{ label: 'Storage', slug: 'operating/storage' },
						{ label: 'Configurations', slug: 'operating/configurations' },
						{ label: 'Machine', slug: 'operating/machine' },
						{ label: 'Diagnostics', slug: 'operating/diagnostics' },
						{ label: 'System', slug: 'operating/system' },
					],
				},
				{
					label: 'Walkthroughs',
					items: [
						{ label: 'Boot XXDP from an emulated RL02', slug: 'walkthroughs/xxdp-rl02' },
						{ label: '2.11BSD on an MSCP disk, on the LAN', slug: 'walkthroughs/211bsd-network' },
						{ label: 'VMS on an emulated VAX-11/780', slug: 'walkthroughs/vax-vms' },
					],
				},
				{
					label: 'Configurations',
					items: [
						// Starlight prefixes the base itself; do not prefix it here.
						{ label: 'Catalogue', link: '/configurations/' },
						{ label: 'Catalogue format', slug: 'configurations/format' },
					],
				},
				{
					label: 'Tools',
					items: [
						{ label: 'The MCP server', slug: 'tools/mcp-server' },
					],
				},
				{
					label: 'Background',
					items: [
						{ label: 'The PDP-11 and its buses', slug: 'background/pdp-11-and-the-buses' },
					],
				},
				{
					label: 'Project',
					items: [
						{ label: 'Glossary', slug: 'project/glossary' },
						{ label: 'FAQ', slug: 'project/faq' },
						{ label: 'What is coming', slug: 'project/roadmap' },
						{ label: 'Credits and licence', slug: 'project/credits' },
					],
				},
			],
		}),
	],
});
