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

/** The two builds. A device's parameters are read once for each. */
const BUSES = ['unibus', 'qbus'];
const BUS_NAME = { unibus: 'UNIBUS', qbus: 'QBUS' };

/**
 * A source as one bus's build compiles it: the lines of a conditional branch the
 * build leaves out are blanked, line numbers kept. Conditions on the bus macros
 * and on what the file itself #defines from numbers are decided; a condition on
 * anything else keeps every branch, which is the safe reading for a document.
 */
function preprocess(text, bus) {
	const defs = new Map([[BUS_NAME[bus], 1]]);
	const value = (expr) => {
		let e = expr
			.replace(/\/\/.*$|\/\*.*?\*\//g, '')
			.replace(/\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)/g, (_, a, b) => (defs.has(a || b) ? '1' : '0'))
			.replace(/\b[A-Za-z_]\w*\b/g, (id) => (defs.has(id) ? String(defs.get(id)) : '?'));
		if (e.includes('?') || !/^[\d\s()!<>=&|+\-*/%]*$/.test(e)) return undefined;
		try {
			return Number(Function(`return (${e});`)());
		} catch {
			return undefined;
		}
	};
	// each open conditional: whether the enclosing text is live, whether a branch
	// was taken, whether the condition could be decided at all
	const stack = [];
	const live = () => stack.every((f) => f.on);
	const out = text.split('\n').map((line) => {
		const d = line.match(/^\s*#\s*(\w+)\s*(.*)$/);
		if (d) {
			const [, dir, rest] = d;
			if (dir === 'if' || dir === 'ifdef' || dir === 'ifndef') {
				const v =
					dir === 'if' ? value(rest) : dir === 'ifdef' ? (defs.has(rest.trim().split(/\s/)[0]) ? 1 : 0)
					: defs.has(rest.trim().split(/\s/)[0]) ? 0 : 1;
				stack.push(v === undefined ? { on: true, taken: false, unknown: true } : { on: !!v, taken: !!v });
			} else if (dir === 'elif' && stack.length) {
				const f = stack[stack.length - 1];
				const v = f.unknown ? undefined : value(rest);
				if (f.unknown || v === undefined) Object.assign(f, { on: true, unknown: true });
				else Object.assign(f, { on: !f.taken && !!v, taken: f.taken || !!v });
			} else if (dir === 'else' && stack.length) {
				const f = stack[stack.length - 1];
				f.on = f.unknown || !f.taken;
			} else if (dir === 'endif') stack.pop();
			else if (dir === 'define' && live()) {
				const m = rest.match(/^(\w+)\s+(.+?)\s*(\/\/.*)?$/);
				if (m && value(m[2]) !== undefined) defs.set(m[1], value(m[2]));
				else if (/^\w+\s*$/.test(rest)) defs.set(rest.trim(), 1);
			}
			return line;
		}
		return live() ? line : '';
	});
	return out.join('\n');
}

/**
 * A source file, with a copy in which comments and the insides of string and
 * character literals are blanked to spaces. Structure — braces, parentheses,
 * semicolons — is found in the masked copy, so a brace in a comment or a string
 * cannot unbalance it; text is taken from the original at the same offsets.
 */
function load(path, bus) {
	const text = preprocess(readFileSync(path, 'utf8'), bus);
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

const files = SOURCE_ROOTS.flatMap((root) => walk(join(REPO, root)));
const SOURCES = Object.fromEntries(BUSES.map((bus) => [bus, files.map((f) => load(f, bus))]));
/** The build being read; collectBoth() sets it for each bus in turn. */
let sources = SOURCES.unibus;

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

/** The arguments of one parameter constructor call, by name. */
function callArgs(type, text, masked, open) {
	if (!ARGS[type]) throw new Error(`parameter type ${type} unknown to ${GENERATOR}`);
	const close = closing(masked, open);
	const raw = splitArgs(text.slice(open + 1, close - 1), masked.slice(open + 1, close - 1));
	return { close, a: Object.fromEntries(ARGS[type].map((k, i) => [k, raw[i]])) };
}

/** The longest common prefix and suffix of some strings, and what varies between. */
function varying(strings) {
	let pre = strings[0];
	let suf = strings[0];
	for (const s of strings) {
		while (!s.startsWith(pre)) pre = pre.slice(0, -1);
		while (!s.endsWith(suf)) suf = suf.slice(1);
	}
	if (pre.length + suf.length > Math.min(...strings.map((s) => s.length))) suf = '';
	return { pre, suf, parts: strings.map((s) => s.slice(pre.length, s.length - suf.length)) };
}

/**
 * An array of parameters, one per line or unit, as one entry: the names it
 * spans, and the description with the varying index written *n*.
 */
function arrayEntry(elems) {
	const first = elems[0];
	const names = elems.map((e) => e.name);
	const info = varying(elems.map((e) => e.info));
	const index = varying(names).parts;
	return {
		...first,
		name: names.length > 1 ? `${names[0]} … ${names[names.length - 1]}` : names[0],
		short: elems.length > 1 && first.short ? `${first.short} … ${elems[elems.length - 1].short}` : first.short,
		info: info.parts.every((p, i) => p === index[i]) && info.parts.some(Boolean) ? `${info.pre}*n*${info.suf}` : first.info,
		count: elems.length,
		lastName: names[names.length - 1],
	};
}

function parameters({ text, masked }, body) {
	const lines = text.split('\n');
	const re = /\bparameter_(\w+)_c\s+(\w+)\s*(\[[^\]]*\])?\s*=\s*(\{|parameter_\w+_c\s*\()/g;
	re.lastIndex = body.start;
	const found = [];
	let m;
	while ((m = re.exec(masked)) && m.index < body.end) {
		const type = m[1];
		const first = lineOf(text, m.index);
		const describe = (a) => ({
			member: m[2],
			type,
			name: cString(a.name),
			short: cString(a.short),
			readonly: a.readonly === 'true',
			unit: a.unit ? cString(a.unit) : '',
			info: cString(a.info),
			base: a.base ? Number(a.base) : 10,
		});
		let entry;
		let close;
		if (m[3]) {
			// an array: { parameter_x_c(...), parameter_x_c(...), ... }
			const open = m.index + m[0].length - 1;
			close = closing(masked, open);
			const call = /parameter_\w+_c\s*\(/g;
			call.lastIndex = open;
			const elems = [];
			let c;
			while ((c = call.exec(masked)) && c.index < close) {
				const r = callArgs(type, text, masked, c.index + c[0].length - 1);
				elems.push(describe(r.a));
				call.lastIndex = r.close;
			}
			entry = arrayEntry(elems);
		} else {
			const r = callArgs(type, text, masked, m.index + m[0].length - 1);
			close = r.close;
			entry = describe(r.a);
		}
		found.push({
			...entry,
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
		for (const s of body.matchAll(/\b(\w+)(?:\[[^\]]*\])?\.(value|kind|readonly|content)\s*=\s*([^;]+);/g))
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
	return [...out.values()]
		.filter((o) => o.readonly !== undefined || o.default !== undefined)
		.map((o) => {
			const bits = [];
			if (o.readonly !== undefined) bits.push(o.readonly ? 'is read-only' : 'can be set');
			if (o.default !== undefined) bits.push(`starts at ${renderDefault({ ...o.param, default: o.default })}`);
			return { ...o, text: `\`${o.param.name}\` ${bits.join(' and ')}` };
		});
}

/**
 * A class as both builds see it, merged. A parameter both carry the same way is
 * one row; where the builds differ the row says how, and a parameter only one
 * build carries says which.
 */
function collectBoth(name, documented) {
	const seen = {};
	for (const bus of BUSES) {
		sources = SOURCES[bus];
		try {
			seen[bus] = collect(name, documented);
		} catch (e) {
			if (!/is not declared/.test(e.message)) throw e;
		}
	}
	sources = SOURCES.unibus;
	const [a, b] = BUSES.map((bus) => seen[bus]);
	if (!a || !b) {
		const only = a ? 'unibus' : 'qbus';
		if (!seen[only]) throw new Error(`class ${name} is not declared under ${SOURCE_ROOTS.join(', ')}`);
		return seen[only];
	}
	const other = new Map(b.params.map((p) => [p.member, p]));
	const params = [];
	for (const p of a.params) {
		const q = other.get(p.member);
		other.delete(p.member);
		if (!q) {
			params.push({ ...p, only: 'unibus' });
			continue;
		}
		const merged = { ...p };
		const da = renderDefault(p);
		const db = renderDefault(q);
		if (da !== db) merged.default = { text: `${da} on UNIBUS, ${db} on QBUS` };
		if (p.readonly !== q.readonly) merged.readonlyOn = p.readonly ? 'unibus' : 'qbus';
		if (p.count !== q.count && p.count && q.count) merged.qbusLast = q.lastName;
		params.push(merged);
	}
	// what only the QBUS build has, after whatever it follows there
	for (const q of other.values()) {
		const at = b.params.indexOf(q);
		const after = at > 0 ? params.findIndex((p) => p.member === b.params[at - 1].member) : -1;
		params.splice(after + 1, 0, { ...q, only: 'qbus' });
	}
	const said = (o) => o.map((x) => x.text).join('; ');
	const oa = said(a.overrides);
	const ob = said(b.overrides);
	return {
		params,
		stops: [...new Set([...a.stops, ...b.stops])],
		overrides: oa === ob ? a.overrides : [...a.overrides.map((x) => ({ ...x, bus: 'unibus' })), ...b.overrides.map((x) => ({ ...x, bus: 'qbus' }))],
	};
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
		const name = p.leader ? `[\`${p.name}\`](#${p.leader.anchor})` : `\`${p.name}\``;
		let kind = valueKind(p);
		if (p.readonlyOn) kind += `, read-only on ${BUS_NAME[p.readonlyOn]}`;
		else if (p.readonly && p.kind === 'config') kind += ', read-only';
		if (p.only) kind += ` (${BUS_NAME[p.only]} only)`;
		if (p.qbusLast) kind += `; on QBUS up to \`${p.qbusLast}\``;
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
	const { params, stops, overrides } = collectBoth(cls, documented);
	// who shares whose `///` text, by member: the builds' copies were merged
	const byMember = new Map(params.map((p) => [p.member, p]));
	for (const p of params) if (p.doc) p.group = [p];
	for (const p of params) {
		p.leader = p.doc ? p : p.shares ? byMember.get(p.shares.member) : null;
		if (p.leader && p.leader !== p) p.leader.group.push(p);
	}

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
		const on = (bus) => overrides.filter((o) => o.bus === bus).map((o) => o.text).join('; ');
		if (overrides.some((o) => o.bus))
			for (const bus of BUSES) {
				if (on(bus)) out.push(`On this device on ${BUS_NAME[bus]}, ${on(bus)}.`, '');
			}
		else out.push(`On this device, ${overrides.map((o) => o.text).join('; ')}.`, '');
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
