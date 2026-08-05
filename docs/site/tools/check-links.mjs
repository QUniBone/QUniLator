#!/usr/bin/env node
/**
 * Check that every internal link in the built site resolves to a file.
 *
 * The base path is baked into absolute links at build time by three different
 * mechanisms — a remark plugin for Markdown, Starlight for sidebar entries, and
 * `import.meta.env.BASE_URL` in .astro pages — and they disagree readily. A link
 * carrying the base twice, or not at all, still builds and still returns HTML;
 * it only fails when somebody clicks it.
 *
 *   node tools/check-links.mjs dist /qunilator
 *   node tools/check-links.mjs dist
 */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';

const dist = resolve(process.argv[2] ?? 'dist');
const base = (process.argv[3] ?? '').replace(/\/+$/, '');

function walk(dir) {
	return readdirSync(dir).flatMap((name) => {
		const full = join(dir, name);
		return statSync(full).isDirectory() ? walk(full) : [full];
	});
}

const files = walk(dist);
const pages = files.filter((f) => f.endsWith('.html'));
/** Every path the built site actually serves, as a site-absolute URL path. */
const served = new Set(
	files.map((f) => {
		const rel = '/' + relative(dist, f).split(/[\\/]/).join('/');
		return base + rel;
	})
);

const problems = [];

for (const page of pages) {
	const html = readFileSync(page, 'utf8');

	// A redirect page carries its destination twice: the `<meta http-equiv=refresh>`
	// that performs it, and an anchor for a reader whose browser does not. Check the
	// meta as well, so the redirect is verified by what actually redirects rather
	// than by the anchor Astro happens to render beside it.
	const targets = [
		...[...html.matchAll(/(?:href|src)="([^"]+)"/g)].map((m) => m[1]),
		...[...html.matchAll(/content="\d+\s*;\s*url=([^"]+)"/gi)].map((m) => m[1]),
	];

	for (const raw of targets) {

		// Pages link to each other by file, and the build turns those into the paths
		// the site serves. One that still names a `.md` file was not converted — the
		// link works in a checkout and 404s here.
		if (!/^[a-z][a-z0-9+.-]*:/i.test(raw) && !raw.startsWith('//')) {
			if (raw.split(/[#?]/)[0].endsWith('.md')) {
				problems.push({ from: relative(dist, page), link: raw, why: 'unconverted .md link' });
				continue;
			}
		}

		// External, protocol-relative, anchors and non-http schemes are not ours.
		if (!raw.startsWith('/') || raw.startsWith('//')) continue;

		const path = raw.split('#')[0].split('?')[0];
		if (!path) continue;

		// A directory URL is served by its index.html.
		const candidates = path.endsWith('/')
			? [path + 'index.html', path.slice(0, -1)]
			: [path, path + '/index.html'];

		if (candidates.some((c) => served.has(c))) continue;

		// A doubled base is the characteristic failure and worth naming.
		const doubled = base && path.startsWith(base + base + '/');
		problems.push({
			from: relative(dist, page),
			link: raw,
			why: doubled ? 'base applied twice' : 'no such file in the build',
		});
	}
}

// A redirect names its destination twice, so report each broken target once.
const seen = new Set();
const unique = problems.filter((p) => {
	const key = `${p.from} ${p.link}`;
	return seen.has(key) ? false : seen.add(key);
});
problems.length = 0;
problems.push(...unique);

if (problems.length === 0) {
	const links = pages.length;
	console.log(`✓ every internal link in ${links} pages resolves` + (base ? ` (base ${base})` : ''));
	process.exit(0);
}

console.error(`✗ ${problems.length} broken internal link(s):\n`);
for (const p of problems) {
	console.error(`  ${p.from}`);
	console.error(`    ${p.link}  — ${p.why}`);
}
process.exit(1);
