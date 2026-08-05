import { relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

/**
 * Where the manual is: `docs/manual/`, beside this generator.
 *
 * The pages sit next to the project's other documentation rather than inside the
 * generator, so the directory reads as a manual to anyone who arrives at it in
 * git or on GitHub. Resolved from this file rather than the working directory, so
 * a build started from anywhere finds it.
 */
export const DOCS_ROOT = fileURLToPath(new URL('../manual', import.meta.url));

/**
 * The same directory as Astro names it in `entry.filePath` — relative to this
 * project, which is what a page carries its own path as. Derived from this file
 * rather than the working directory, which during prerendering is neither.
 */
export const DOCS_ROOT_RELATIVE =
	relative(fileURLToPath(new URL('.', import.meta.url)), DOCS_ROOT).split(sep).join('/') + '/';

/** The address the manual links to pages that only the site can serve. */
export const CANONICAL_SITE = 'https://qunilator.com';

/** Where "Edit this page" sends a reader, one page below. */
export const MANUAL_EDIT_BASE = 'https://github.com/QUniBone/QUniLator/edit/main/docs/manual/';
