import type { APIRoute } from 'astro';
import { getCollection } from 'astro:content';

/**
 * The catalogue index a board subscribes to.
 *
 * A static file rather than an endpoint: it is the cheapest thing to host, cache
 * and mirror, and a user group publishing their own catalogue needs a web server
 * and nothing else. The human pages under /configurations/ render from the same
 * collection, so a catalogue entry and its documentation cannot drift apart.
 *
 * Served with permissive CORS (see public/_headers) because the board may sit on
 * an isolated LAN with no route out; in that case the operator's browser fetches
 * the index and posts what it got.
 */
export const GET: APIRoute = async ({ site }) => {
	const entries = await getCollection('configurations');
	const base = import.meta.env.BASE_URL.replace(/\/+$/, '');

	const body = {
		schema: 'qunilator-catalog/1',
		name: "The QUniLator project's own catalogue",
		updated: entries
			.map((e) => e.data.doc.added.toISOString().slice(0, 10))
			.sort()
			.at(-1),
		configurations: entries
			.map((entry) => ({
				id: entry.id,
				title: entry.data.title,
				summary: entry.data.summary,
				bus: entry.data.bus,
				devices: entry.data.devices,
				guest: entry.data.guest,
				// Where a reader goes for the full documentation of this entry.
				page: new URL(`${base}/configurations/${entry.id}/`, site).href,
				download: entry.data.download,
				doc: {
					...entry.data.doc,
					added: entry.data.doc.added.toISOString().slice(0, 10),
				},
			}))
			.sort((a, b) => a.id.localeCompare(b.id)),
	};

	return new Response(JSON.stringify(body, null, 2), {
		headers: { 'Content-Type': 'application/json; charset=utf-8' },
	});
};
