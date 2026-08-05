/**
 * Turn the links a Markdown file can be read with into the links the site serves.
 *
 * Pages link to each other by file — `../start/install.md#reading-the-leds` — so
 * that a link works in a checkout, on GitHub, and in any editor's preview. The
 * site serves those pages as directories, and from a subdirectory when it is
 * published under a base path, so both of those rewrites happen here.
 *
 *   ../start/install.md#reading-the-leds  ->  /qunilator/start/install/#reading-the-leds
 *   https://qunilator.com/configurations/ ->  /qunilator/configurations/
 *   /catalog/v1/index.json                ->  /qunilator/catalog/v1/index.json
 *
 * Pages that only exist on the site — the catalogue, generated from YAML — cannot
 * be named by file, so content links to them at their canonical address. Folding
 * that back to a local path keeps a subdirectory deployment linked to itself
 * rather than bouncing readers to the canonical site.
 *
 * Image sources are left exactly as written: Astro resolves relative images
 * itself, and optimises the ones it can resolve.
 */
import { dirname, relative, resolve, sep } from 'node:path';
import { visit } from 'unist-util-visit';

export function remarkDocLinks({ docsRoot, base, canonical }) {
	const prefix = base.replace(/\/+$/, '');
	const home = canonical.replace(/\/+$/, '');

	/** Prefix a root-absolute path with the base, tolerating a path that carries it already. */
	const based = (path) => {
		if (!prefix) return path;
		if (path === prefix || path.startsWith(prefix + '/')) return path;
		return prefix + path;
	};

	/** The path the site serves a docs file at, or null if the file is outside the tree. */
	const served = (file) => {
		const rel = relative(docsRoot, file).split(sep).join('/');
		if (!rel || rel.startsWith('../')) return null;
		const page = rel.replace(/\.mdx?$/, '').replace(/(^|\/)index$/, '');
		return based(page ? `/${page}/` : '/');
	};

	return () => (tree, vfile) => {
		visit(tree, ['link', 'definition'], (node) => {
			const url = node.url;
			if (typeof url !== 'string' || !url || url.startsWith('#')) return;

			if (url === home || url.startsWith(home + '/')) {
				node.url = based(url.slice(home.length) || '/');
				return;
			}

			// Anything with a scheme, and protocol-relative URLs, belong to somebody else.
			if (url.startsWith('//') || /^[a-z][a-z0-9+.-]*:/i.test(url)) return;

			if (url.startsWith('/')) {
				node.url = based(url);
				return;
			}

			const hash = url.indexOf('#');
			const path = hash === -1 ? url : url.slice(0, hash);
			if (!/\.mdx?$/.test(path)) return;

			// A link to a file the site does not serve is left alone; the built page
			// then carries a visibly broken `.md` link, which `tools/check-links.mjs`
			// reports rather than letting it reach a reader.
			const target = vfile.path && served(resolve(dirname(vfile.path), path));
			if (target) node.url = hash === -1 ? target : target + url.slice(hash);
		});
	};
}
