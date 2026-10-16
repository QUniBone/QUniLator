#!/usr/bin/env node
/**
 * Writes the parameter reference of each device page from the C++ source.
 *
 *   node tools/device-params.mjs           rewrite the pages
 *   node tools/device-params.mjs --check   fail if a page is out of date
 *
 * A page under `docs/manual/devices/` carries a marked block for each class it
 * documents, under a heading of the author's:
 *
 *   ## Parameters
 *
 *   <!-- device-parameters: cpuvax_c -->
 *   <!-- /device-parameters -->
 *
 * Everything between the two markers is replaced by tables of the class's
 * parameters, read from where they are declared: name, short name, type, unit,
 * read-only flag and the one-line description the web interface shows. The
 * constructor supplies defaults and which parameters are running status. A
 * `///` comment directly above a declaration is the longer explanation and is
 * written under the tables; declarations that follow it with no blank line in
 * between share it. A `/// @default <text>` line in it fills the Default column
 * for a parameter whose starting value is not a literal in a constructor.
 *
 * Base classes are walked and their parameters included, up to a class that has
 * a block of its own, which is linked instead: the link names the heading that
 * block sits under. So `qunibusdevice_c`, documented once on the overview page,
 * is not repeated on every device's. Generated headings sit one level below the
 * author's heading above the block.
 */
import { readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { DOCS_ROOT } from '../docs-root.mjs';

const REPO = fileURLToPath(new URL('../../..', import.meta.url));
const DEVICES_DIR = join(DOCS_ROOT, 'devices');
const GENERATOR = 'docs/site/tools/device-params.mjs';

/** Where device classes are declared. */
const SOURCE_ROOTS = ['10.01_base/2_src/arm', '10.02_devices/2_src'];

/**
 * What a device is called, what type it is and whether it is switched on are not
 * in its Parameters list on the Configurations screen either; the overview page
 * describes them.
 */
const IDENTITY = new Set(['name', 'type', 'enabled']);

const END = '<!-- /device-parameters -->';

// ---- sources ------------------------------------------------------------

function walk(dir, out = []) {
	for (const name of readdirSync(dir)) {
		const path = join(dir, name);
		if (statSync(path).isDirectory()) walk(path, out);
		else if (/\.(hpp|h|cpp)$/.test(name)) out.push(path);
	}
	return out;
}

/**
 * A source file, with a copy in which comments and the insides of string and
 * character literals are blanked to spaces. Structure — braces, parentheses,
 * semicolons — is found in the masked copy, so a brace in a comment or a string
 * cannot unbalance it; text is taken from the original at the same offsets.
 */
function load(path) {
	const text = readFileSync(path, 'utf8');
	const m = text.split('');
	for (let i = 0; i < m.length; ) {
		if (text.startsWith('//', i)) {
			while (i < m.length && text[i] !== '\n') m[i++] = ' ';
		} else if (text.startsWith('/*', i)) {
			const end = text.indexOf('*/', i + 2);
			const stop = end < 0 ? m.length : end + 2;
			for (; i < stop; i++) if (text[i] !== '\n') m[i] = ' ';
		} else if (text[i] === '"' || text[i] === "'") {
			const q = text[i++];
			while (i < m.length && text[i] !== q && text[i] !== '\n') {
				if (text[i] === '\\') m[i++] = ' ';
				m[i++] = ' ';
			}
			i++;
		} else i++;
	}
	return { path, text, masked: m.join('') };
}

const sources = SOURCE_ROOTS.flatMap((root) => walk(join(REPO, root))).map(load);

/** Offset just past the bracket that closes the one at `open`. */
function closing(masked, open) {
	const pair = { '{': '}', '(': ')' }[masked[open]];
	let depth = 0;
	for (let i = open; i < masked.length; i++) {
		if (masked[i] === masked[open]) depth++;
		else if (masked[i] === pair && --depth === 0) return i + 1;
	}
	throw new Error(`unbalanced ${masked[open]} at offset ${open}`);
}

/** Top-level comma-separated pieces of an argument list, as source text. */
function splitArgs(src, masked) {
	const args = [];
	let depth = 0;
	let start = 0;
	for (let i = 0; i < masked.length; i++) {
		const c = masked[i];
		if ('([{'.includes(c)) depth++;
		else if (')]}'.includes(c)) depth--;
		else if (c === ',' && depth === 0) {
			args.push(src.slice(start, i));
			start = i + 1;
		}
	}
	args.push(src.slice(start));
	return args.map((a) => a.replace(/\/\*[\s\S]*?\*\//g, '').trim());
}

/** A C string expression — adjacent literals concatenated — as its value. */
function cString(expr) {
	const parts = [...expr.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1]);
	return parts.join('').replace(/\\(.)/g, (_, c) => ({ n: '\n', t: '\t' })[c] ?? c);
}

const lineOf = (text, offset) => text.slice(0, offset).split('\n').length - 1;

// ---- a class ------------------------------------------------------------

/**
 * The declaration of a class: where it is, its bases, its parameters. Null for a
 * class declared outside the source roots, which is infrastructure every device
 * shares — logging, the parameter list itself — and has no parameters to show.
 */
function findClass(name) {
	const re = new RegExp(`\\bclass\\s+${name}\\s*(?::([^{;]*))?\\{`, 'g');
	for (const src of sources) {
		const m = re.exec(src.masked);
		re.lastIndex = 0;
		if (!m) continue;
		const open = m.index + m[0].length - 1;
		const body = { start: open + 1, end: closing(src.masked, open) - 1 };
		const bases = (m[1] || '')
			.split(',')
			.map((b) => b.replace(/\b(public|protected|private|virtual)\b/g, '').trim())
			.filter(Boolean);
		return { name, src, bases, params: parameters(src, body) };
	}
	return null;
}

const ARGS = {
	string: ['parent', 'name', 'short', 'readonly', 'info'],
	bool: ['parent', 'name', 'short', 'readonly', 'info'],
	unsigned: ['parent', 'name', 'short', 'readonly', 'unit', 'format', 'info', 'bitwidth', 'base'],
	unsigned64: ['parent', 'name', 'short', 'readonly', 'unit', 'format', 'info', 'bitwidth', 'base'],
	double: ['parent', 'name', 'short', 'readonly', 'unit', 'format', 'info'],
};

function parameters({ text, masked }, body) {
	const lines = text.split('\n');
	const re = /\bparameter_(\w+)_c\s+(\w+)\s*=\s*parameter_\w+_c\s*\(/g;
	re.lastIndex = body.start;
	const found = [];
	let m;
	while ((m = re.exec(masked)) && m.index < body.end) {
		const type = m[1];
		if (!ARGS[type]) throw new Error(`parameter type ${type} unknown to ${GENERATOR}`);
		const open = m.index + m[0].length - 1;
		const close = closing(masked, open);
		const raw = splitArgs(text.slice(open + 1, close - 1), masked.slice(open + 1, close - 1));
		const a = Object.fromEntries(ARGS[type].map((k, i) => [k, raw[i]]));
		const first = lineOf(text, m.index);
		found.push({
			member: m[2],
			type,
			name: cString(a.name),
			short: cString(a.short),
			readonly: a.readonly === 'true',
			unit: a.unit ? cString(a.unit) : '',
			info: cString(a.info),
			base: a.base ? Number(a.base) : 10,
			...docComment(lines, first),
			first,
			last: lineOf(text, close),
			kind: 'config',
			content: 'plain',
			default: undefined,
		});
		re.lastIndex = close;
	}
	// A `///` block covers the declarations that run on from it without a blank
	// line; a maintainer's `//` comment between them does not break the run.
	const onlyComments = (from, to) => lines.slice(from, to).every((l) => /^\s*\/\/(?!\/)/.test(l));
	for (let i = 1; i < found.length; i++) {
		const p = found[i];
		const prev = found[i - 1];
		const leader = prev.shares || (prev.doc ? prev : null);
		if (!p.doc && leader && onlyComments(prev.last + 1, p.first)) {
			p.shares = leader;
			p.defaultText ??= leader.defaultText;
		}
	}
	return found;
}

/** The `///` lines directly above a declaration: paragraphs of text and an @default. */
function docComment(lines, decl) {
	const block = [];
	for (let i = decl - 1; i >= 0 && /^\s*\/\//.test(lines[i]); i--) block.unshift(lines[i].trim());
	let doc = block.filter((l) => l.startsWith('///')).map((l) => l.replace(/^\/\/\/ ?/, ''));
	const tag = doc.find((l) => l.startsWith('@default '));
	doc = doc.filter((l) => l !== tag);
	return { doc: prose(doc), defaultText: tag ? tag.slice('@default '.length).trim() : undefined };
}

function prose(doc) {
	if (!doc.length) return '';
	return doc
		.join('\n')
		.split(/\n\s*\n/)
		.map((para) => para.replace(/\s*\n\s*/g, ' ').trim())
		.filter(Boolean)
		.join('\n\n');
}

/** What the constructor of a class sets on its parameters. */
function constructorSettings(name) {
	const re = new RegExp(`\\b${name}::${name}\\s*\\(`, 'g');
	for (const { text, masked } of sources) {
		const m = re.exec(masked);
		re.lastIndex = 0;
		if (!m) continue;
		const afterArgs = closing(masked, m.index + m[0].length - 1);
		const open = masked.indexOf('{', afterArgs);
		const body = text.slice(open, closing(masked, open));
		const sets = [];
		for (const s of body.matchAll(/\b(\w+)\.(value|kind|readonly|content)\s*=\s*([^;]+);/g))
			sets.push({ member: s[1], field: s[2], expr: s[3].trim() });
		return sets;
	}
	return [];
}

/** A C literal as the value it denotes, or undefined for anything else. */
function literal(expr, type) {
	if (type === 'bool') return { true: true, false: false, 1: true, 0: false }[expr];
	if (type === 'string') return /^"(?:[^"\\]|\\.)*"$/.test(expr) ? cString(expr) : undefined;
	const n = expr.replace(/[uUlL]+$/, '');
	if (type === 'double') return /^[-+]?\d+(\.\d*)?([eE][-+]?\d+)?$/.test(n) ? Number(n) : undefined;
	if (/^0[xX][0-9a-fA-F]+$/.test(n)) return parseInt(n, 16);
	if (/^0[0-7]+$/.test(n)) return parseInt(n, 8);
	if (/^\d+$/.test(n)) return parseInt(n, 10);
	return undefined;
}

/**
 * The parameters a page lists for a class: its own and its bases', up to a class
 * documented elsewhere. Each constructor on the way applies its settings to the
 * parameters declared at or above it, so a subclass's default wins.
 */
function collect(name, documented) {
	const chain = [];
	const stops = [];
	const visit = (cls) => {
		const c = findClass(cls);
		if (!c) return;
		chain.push(c);
		for (const b of c.bases) {
			if (documented.has(b)) stops.push(b);
			else visit(b);
		}
	};
	visit(name);
	if (!chain.length) throw new Error(`class ${name} is not declared under ${SOURCE_ROOTS.join(', ')}`);
	// base classes first, as a device's parameters are listed by the API
	const params = [...chain].reverse().flatMap((c) => c.params);
	const byMember = new Map(params.map((p) => [p.member, p]));
	// base constructors run first, so apply them first
	for (const c of [...chain].reverse()) {
		for (const s of constructorSettings(c.name)) {
			const p = byMember.get(s.member);
			if (!p) continue;
			if (s.field === 'value') p.default = literal(s.expr, p.type);
			if (s.field === 'kind') p.kind = /PARAM_STATUS/.test(s.expr) ? 'status' : 'config';
			if (s.field === 'readonly') p.readonly = s.expr === 'true';
			if (s.field === 'content') p.content = (s.expr.match(/CONTENT_(\w+)/)?.[1] || 'plain').toLowerCase();
		}
	}
	for (const p of params) {
		if (p.defaultText) p.default = { text: p.defaultText };
		else if (p.default === undefined && !constructorSets(chain, p.member))
			p.default = { string: '', bool: false, double: 0 }[p.type] ?? 0;
	}
	return {
		params: params.filter((p) => !IDENTITY.has(p.name)),
		stops: [...new Set(stops)],
		overrides: overrides(chain, [...new Set(stops)]),
	};
}

/**
 * What the class's constructors change about a parameter it inherits from a
 * class documented elsewhere: a literal starting value, or whether it is
 * read-only. Listed on the page, since the other page shows the general case.
 */
function overrides(chain, stops) {
	const inherited = new Map();
	for (const stop of stops)
		for (const p of collect(stop, new Set()).params) inherited.set(p.member, p);
	const out = new Map();
	for (const c of chain) {
		for (const s of constructorSettings(c.name)) {
			const p = inherited.get(s.member);
			if (!p) continue;
			const o = out.get(p.name) || { param: p };
			if (s.field === 'readonly') o.readonly = s.expr === 'true';
			if (s.field === 'value') {
				const v = literal(s.expr, p.type);
				if (v !== undefined) o.default = v;
			}
			out.set(p.name, o);
		}
	}
	return [...out.values()].filter((o) => o.readonly !== undefined || o.default !== undefined);
}

/** Whether some constructor assigns the parameter's value, literal or not. */
const constructorSets = (chain, member) =>
	chain.some((c) => constructorSettings(c.name).some((s) => s.member === member && s.field === 'value'));

// ---- rendering ----------------------------------------------------------

/** The anchor Starlight gives a heading (github-slugger, for ASCII text). */
const slug = (heading) =>
	heading
		.toLowerCase()
		.replace(/`/g, '')
		.replace(/[^a-z0-9 _-]/g, '')
		.replace(/ /g, '-');

const cell = (s) => String(s).replace(/\|/g, '\\|').replace(/\n/g, ' ');

function valueKind(p) {
	let k;
	if (p.type === 'bool') k = '0 or 1';
	else if (p.type === 'string') k = { image: 'image file', rom: 'ROM file' }[p.content] || 'text';
	else if (p.type === 'double') k = 'number';
	else k = { 8: 'octal', 16: 'hex' }[p.base] || 'number';
	if (p.unit) k += `, ${p.unit}`;
	return k;
}

function renderDefault(p) {
	const v = p.default;
	if (v === undefined) return '—';
	if (v.text) return cell(v.text);
	if (p.type === 'bool') return v ? '1' : '0';
	if (p.type === 'string') return v === '' ? '*empty*' : `\`${v}\``;
	if (p.type === 'double') return String(v);
	return String(v.toString(p.base === 8 ? 8 : p.base === 16 ? 16 : 10));
}

const heading = (group) => group.map((p) => `\`${p.name}\``).join(', ');

function table(params, { withDefault }) {
	const rows = params.map((p) => {
		const leader = p.shares || (p.doc ? p : null);
		const name = leader ? `[\`${p.name}\`](#${slug(heading(leader.group))})` : `\`${p.name}\``;
		const kind = valueKind(p) + (p.readonly && p.kind === 'config' ? ', read-only' : '');
		const cols = [name, p.short && p.short !== p.name ? `\`${p.short}\`` : '', kind];
		if (withDefault) cols.push(renderDefault(p));
		cols.push(cell(p.info));
		return `| ${cols.join(' | ')} |`;
	});
	const head = withDefault
		? '| Parameter | Short | Value | Default | What it is |\n|---|---|---|---|---|'
		: '| Parameter | Short | Value | What it is |\n|---|---|---|---|';
	return `${head}\n${rows.join('\n')}`;
}

/**
 * Starlight's heading anchors for one page, in document order: github-slugger,
 * which numbers a repeated heading -1, -2 and so on. A page of several blocks
 * repeats "Settings" and "Status", so every anchor is taken from here, in the
 * order the headings are written.
 */
function slugger() {
	const seen = new Map();
	return (text) => {
		const base = slug(text);
		const n = seen.get(base) || 0;
		seen.set(base, n + 1);
		return n ? `${base}-${n}` : base;
	};
}

/** A heading line's level and text, or null. */
function headingLine(line) {
	const m = line.match(/^(#{1,6})\s+(.*?)\s*#*\s*$/);
	return m ? { level: m[1].length, text: m[2] } : null;
}

/**
 * The parameter reference of one class, under a heading of `level`. `anchor`
 * takes each generated heading in order and returns its anchor; `link` turns a
 * class documented elsewhere into a Markdown link.
 */
function render(cls, documented, level, anchor, link) {
	const { params, stops, overrides } = collect(cls, documented);
	for (const p of params) if (p.doc) p.group = [p];
	for (const p of params) if (p.shares) p.shares.group.push(p);

	const settings = params.filter((p) => p.kind === 'config');
	const status = params.filter((p) => p.kind === 'status');
	const notes = params.filter((p) => p.doc);
	const h = '#'.repeat(Math.min(level + 1, 6));
	const hh = '#'.repeat(Math.min(level + 2, 6));

	// Anchors in the order the headings appear, before the tables that link to them.
	if (settings.length) anchor('Settings');
	if (status.length) anchor('Status');
	if (notes.length) anchor('In detail');
	for (const p of notes) p.anchor = anchor(heading(p.group));

	const out = [];
	if (settings.length) {
		out.push(`${h} Settings`, '', 'What a configuration stores and an operator sets.', '');
		out.push(table(settings, { withDefault: true }), '');
	}
	if (status.length) {
		out.push(`${h} Status`, '', 'What the device reports as it runs. A configuration does not store these.', '');
		out.push(table(status, { withDefault: false }), '');
	}
	if (!settings.length && !status.length) out.push('It has no parameters of its own.', '');
	for (const stop of stops) out.push(`It also carries the parameters under ${link(stop)}.`, '');
	if (overrides.length) {
		const said = overrides.map((o) => {
			const bits = [];
			if (o.readonly !== undefined) bits.push(o.readonly ? 'is read-only' : 'can be set');
			if (o.default !== undefined) bits.push(`starts at ${renderDefault({ ...o.param, default: o.default })}`);
			return `\`${o.param.name}\` ${bits.join(' and ')}`;
		});
		out.push(`On this device, ${said.join('; ')}.`, '');
	}
	if (notes.length) {
		out.push(`${h} In detail`, '');
		for (const p of notes) out.push(`${hh} ${heading(p.group)}`, '', p.doc, '');
	}
	return out.join('\n').trimEnd();
}

// ---- pages --------------------------------------------------------------

const MARKER = /<!-- device-parameters: *(\w+) *-->/g;

/**
 * Where each class is documented: the page, and the heading its block sits
 * under, which is what a link to it names.
 */
const homes = new Map();
const pagePaths = readdirSync(DEVICES_DIR)
	.filter((f) => f.endsWith('.md'))
	.map((f) => join(DEVICES_DIR, f));
for (const path of pagePaths) {
	const text = readFileSync(path, 'utf8');
	const anchor = slugger();
	let under = null;
	for (const line of text.split('\n')) {
		const hd = headingLine(line);
		if (hd) under = { ...hd, anchor: anchor(hd.text) };
		for (const m of line.matchAll(MARKER)) {
			if (homes.has(m[1]))
				throw new Error(`${m[1]} is documented twice: ${relative(REPO, homes.get(m[1]).path)} and ${relative(REPO, path)}`);
			if (!under) throw new Error(`${relative(REPO, path)}: the ${m[1]} block has no heading above it`);
			homes.set(m[1], { path, heading: under });
		}
	}
}

/** One page with every block in it written afresh. */
function regenerate(path) {
	const text = readFileSync(path, 'utf8');
	const lines = text.split('\n');
	const anchor = slugger();
	const out = [];
	let level = 1;
	for (let i = 0; i < lines.length; i++) {
		const line = lines[i];
		const m = line.match(/^<!-- device-parameters: *(\w+) *-->$/);
		if (!m) {
			const hd = headingLine(line);
			if (hd) {
				level = hd.level;
				anchor(hd.text);
			}
			out.push(line);
			continue;
		}
		const end = lines.indexOf(END, i);
		if (end < 0) throw new Error(`${relative(REPO, path)}: the ${m[1]} block has no ${END}`);
		const cls = m[1];
		const others = new Set([...homes.keys()].filter((c) => c !== cls));
		const link = (other) => {
			const home = homes.get(other);
			const page = home.path === path ? '' : relative(join(path, '..'), home.path).split('\\').join('/');
			const name = home.heading.text.replace(/^./, (c) => c.toLowerCase());
			return `[${name}](${page}#${home.heading.anchor})`;
		};
		out.push(
			line,
			`<!-- Written by ${GENERATOR} from the C++ source. Edit the source, then run it. -->`,
			'',
			render(cls, others, level, anchor, link),
			'',
			END,
		);
		i = end;
	}
	return out.join('\n');
}

const check = process.argv.includes('--check');
let stale = 0;
for (const path of pagePaths) {
	const text = readFileSync(path, 'utf8');
	if (!MARKER.test(text)) continue;
	MARKER.lastIndex = 0;
	const next = regenerate(path);
	const rel = relative(REPO, path);
	if (next === text) continue;
	if (check) {
		console.error(`${rel} is out of date: run node ${GENERATOR}`);
		stale++;
	} else {
		writeFileSync(path, next);
		console.log(`wrote ${rel}`);
	}
}
process.exit(stale ? 1 : 0);
