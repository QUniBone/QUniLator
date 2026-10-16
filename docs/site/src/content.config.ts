import { defineCollection } from 'astro:content';
import { z } from 'zod';
import { docsSchema } from '@astrojs/starlight/schema';
import { glob } from 'astro/loaders';
import { DOCS_ROOT } from '../docs-root.mjs';

/** The two buses a board can carry. A page or configuration declares which it applies to. */
export const busEnum = z.enum(['unibus', 'qbus']);

/**
 * Structured configuration documentation, field for field as QUniLator issue #81
 * defines it. The board validates and renders the same shape, so a catalogue entry
 * here and a `readme.md` inside a `.qcfg.zip` carry the same information.
 */
const configurationDoc = z.object({
	motivation: z.string(),
	usage: z.array(z.string()),
	bugs: z.array(z.string()).default([]),
	links: z.array(z.object({ label: z.string(), url: z.url() })).default([]),
	maintainer: z.object({ name: z.string(), contact: z.string() }),
	added: z.date(),
});

export const collections = {
	/**
	 * The manual, from `docs/` at the top of the repository. Starlight's own
	 * `docsLoader()` reads `src/content/docs/` and takes no other base, so the
	 * collection uses a plain glob against the same filenames it would match.
	 */
	docs: defineCollection({
		// `README.md` is the contents page for anyone reading the manual in git; the
		// site builds its navigation from the sidebar instead.
		loader: glob({ base: DOCS_ROOT, pattern: ['**/[^_]*.md', '!README.md'] }),
		schema: docsSchema({
			extend: z.object({
				/**
				 * Which bus this page applies to. Both by default: most of the manual is
				 * shared, and only the pages that genuinely differ narrow themselves.
				 */
				bus: z.array(busEnum).default(['unibus', 'qbus']),
			}),
		}),
	}),

	/**
	 * The project's own configuration catalogue. Each entry renders as a page under
	 * /configurations/ and as one record in /catalog/v1/index.json, which is what a
	 * board subscribes to. One source, both readers.
	 */
	configurations: defineCollection({
		loader: glob({ pattern: '**/*.yaml', base: './src/content/configurations' }),
		schema: z.object({
			title: z.string(),
			summary: z.string(),
			bus: busEnum,
			/**
			 * What the backplane must carry beyond the bus: the processor class,
			 * and any memory or console demand worth a warning. Free text — the
			 * board cannot sense what CPU sits in a backplane, so this is for the
			 * operator's judgement, where `bus` is checked by the machine.
			 */
			cpu: z.string().optional(),
			/** Emulated cards the configuration installs, in layout order. */
			devices: z.array(z.string()),
			/** Guest operating system on the pack, when the configuration boots one. */
			guest: z.string().optional(),
			doc: configurationDoc,
			/**
			 * The published `.qcfg.zip`. Media runs to hundreds of megabytes, so the
			 * bundle is hosted as a release asset rather than committed here; `bytes`
			 * and `sha256` are what the board verifies the download against.
			 */
			download: z.object({
				url: z.url(),
				bytes: z.number().int().positive(),
				sha256: z.string().regex(/^[0-9a-f]{64}$/),
			}),
			/**
			 * The images inside the bundle, as images-root subpaths with their
			 * sizes — what lets a board say which are already there and how much
			 * space an import still needs before it downloads anything.
			 */
			images: z
				.array(z.object({ path: z.string(), bytes: z.number().int().positive() }))
				.default([]),
		}),
	}),
};
