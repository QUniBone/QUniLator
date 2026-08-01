// Configuration management as a master/detail screen at /config.
//
//   /config                     master list, empty detail
//   /config/<name>              detail for <name>: its devices + image assignments
//   /config/<name>/<device>     the same, with <device>'s parameters expanded
//   ?show=all                   ephemeral filter: reveal disabled devices too
//
// One editor serves every configuration; the difference is what Save does, and
// it is signalled by the header, not a mode:
//   - the CURRENT configuration edits the running machine live (via /api/devices)
//     and Save writes the live setup back to the file (?from=live);
//   - a STORED configuration is staged in the editor and reaches nothing until
//     Save writes the whole document (no flag).
import { html } from '../html';
import { useState, useEffect } from 'preact/hooks';
import { useRoute, useLocation } from 'preact-iso';
import { esc, octalStr } from '../lib/util';
import { confirmModal, promptModal, pickDevice } from '../lib/modals';
import { setNavGuard, clearNavGuard, guardedRoute } from '../lib/navguard';
import {
  loadConfigs,
  refreshDevices,
  liveSetParam,
  fetchConfigSnapshot,
  saveConfigFromLive,
  saveConfigDoc,
  applyConfig,
  liveControl,
  renameConfig,
  setConfigTitle,
  setConfigDip,
  deleteConfig,
} from '../api';
import { keptDevices } from '../lib/devmodel';
import { serialEndpoint, serialLines } from '../lib/serial';
import type { SerialLine, SerialRole } from '../lib/serial';
import { useStore } from '../store';
import { Chip, ImageField, DelButton } from './common';
import type { LiveDev, LiveParam, ConfigSnapshot, ConfigSummary } from '../types';

// ---- staged edits of a stored document ----
interface Staged {
  enabled: Record<string, boolean>;
  params: Record<string, Record<string, string>>;
}
function seedStaged(doc: ConfigSnapshot): Staged {
  const st: Staged = { enabled: {}, params: {} };
  (doc.devices || []).forEach((d) => {
    st.enabled[d.name] = d.enabled;
    st.params[d.name] = { ...(d.params || {}) };
  });
  return st;
}
// The staged document written by a stored-config Save: every staged-enabled
// device with the parameters staged for it (untouched defaults are omitted, so
// the backend fills them in). A drive is written only under a controller the
// document carries, so a removed controller takes its drives — and the media
// they named — out of the document with it.
function serialize(st: Staged): ConfigSnapshot {
  const devices = keptDevices((d) => !!st.enabled[d.name]).map((d) => ({
    name: d.name,
    enabled: true,
    params: st.params[d.name] || {},
  }));
  return { devices };
}

// ---- the row model both modes render ----
interface Row {
  name: string;
  label: string;
  type: string;
  enabled: boolean;
  takesImage: boolean;
  image: string;
  params: LiveParam[]; // settable/display params (image handled separately)
  drives: Row[];
  addressOptions?: number[];
  vectorOptions?: number[];
}
// A device's bus placement — where it sits in the I/O page and how it
// interrupts. Editable as a menu of the type's standard values; the backend
// gates a live change on the CPU being halted.
const BUS_PLACEMENT = new Set(['base_addr', 'intr_vector', 'intr_level']);
function liveRow(d: LiveDev): Row {
  return {
    name: d.name,
    label: d.label || d.name,
    type: d.type,
    enabled: d.enabled,
    takesImage: d.params.some((p) => p.n === 'image'),
    image: d.img,
    params: d.params,
    drives: (d.drives || []).map(liveRow),
    addressOptions: d.addressOptions,
    vectorOptions: d.vectorOptions,
  };
}
function storedRow(d: LiveDev, st: Staged): Row {
  const pv = st.params[d.name] || {};
  return {
    name: d.name,
    label: d.label || d.name,
    type: d.type,
    enabled: !!st.enabled[d.name],
    takesImage: d.params.some((p) => p.n === 'image'),
    image: 'image' in pv ? pv.image : d.img,
    // bus-placement fields read back read-only on a live enabled device, but the
    // configuration still edits them, so they are kept rather than filtered
    params: d.params
      .filter((p) => (!p.ro || BUS_PLACEMENT.has(p.n)) && p.n !== 'image')
      .map((p) => ({ ...p, v: p.n in pv ? pv[p.n] : p.v })),
    drives: (d.drives || []).map((c) => storedRow(c, st)),
    addressOptions: d.addressOptions,
    vectorOptions: d.vectorOptions,
  };
}

type SetEnabled = (name: string, on: boolean) => void;
type SetParam = (name: string, param: string, value: string) => void;
type SetImage = (name: string, image: string) => void;

// A plain-language read-back of what a serial field's text does, translated by
// the same pure endpoint parser the field writes with.
function endpointHint(text: string): string {
  const ep = serialEndpoint.parse(text);
  if (ep.role === 'websocket') return 'browser terminal (websocket)';
  if (!ep.port) return 'line off';
  return ep.role === 'connect'
    ? 'connects to ' + ep.host + ':' + ep.port
    : 'listens on port ' + ep.port;
}

// One serial line configured as a single field. It reads the line's three
// backend parameters (role/host/port), formats them into one endpoint string,
// and on change parses the field back into those three parameters — writing
// through the same onParam path the rest of the editor uses (live for the
// current configuration, staged for a stored one). It never evaluates the
// endpoint; it only translates the field.
function SerialPortField({ row, line, onParam }: { row: Row; line: SerialLine; onParam: SetParam }) {
  const param = (n: string) => row.params.find((q) => q.n === n);
  const val = (n: string) => {
    const p = param(n);
    return p ? p.v : '';
  };
  const text = serialEndpoint.format({
    role: val(line.roleParam) as SerialRole,
    host: val(line.hostParam),
    port: parseInt(val(line.portParam), 10) || 0,
  });
  // a running mux fixes its line configuration; the backend marks the parameters
  // read-only, so the field reflects the value without offering an edit
  const ro = !!(param(line.portParam) || {}).ro;
  const [draft, setDraft] = useState(text);
  useEffect(() => setDraft(text), [text]);
  const commit = () => {
    const ep = serialEndpoint.parse(draft);
    onParam(row.name, line.roleParam, ep.role);
    onParam(row.name, line.hostParam, ep.host);
    onParam(row.name, line.portParam, String(ep.port));
  };
  return html`<div class="serial-line">
    <span class="serial-line-name mono">${line.label}</span>
    <input class="serial-line-input mono" type="text" value=${draft} placeholder="port, host:port, or ws"
      disabled=${ro}
      onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
      onChange=${commit} onBlur=${commit} />
    <span class="serial-line-hint muted">${endpointHint(draft)}</span>
  </div>`;
}

// The standard-value menu for a bus-placement parameter, formatted to match the
// parameter's own display (octal, padded to its bitwidth). null for any other
// parameter. The current value is always included, so a value off the standard
// list (an operator override) stays visible and selected.
function placementOptions(row: Row, p: LiveParam): string[] | null {
  let nums: number[] | undefined;
  if (p.n === 'base_addr') nums = row.addressOptions;
  else if (p.n === 'intr_vector') nums = row.vectorOptions;
  else if (p.n === 'intr_level') nums = [4, 5, 6, 7];
  if (!nums) return null;
  const strs = nums.map((n) => octalStr(n, p.bw));
  if (!strs.includes(p.v)) strs.unshift(p.v);
  return strs;
}

function paramControl(row: Row, p: LiveParam, onParam: SetParam) {
  const placement = placementOptions(row, p);
  if (placement)
    return html`<select onChange=${(e: Event) =>
      onParam(row.name, p.n, (e.target as HTMLSelectElement).value)}>
      ${placement.map((s) => html`<option value=${s} selected=${s === p.v}>${s}</option>`)}</select>`;
  if (p.ro) return html`<span class="ro">${p.v}</span>`;
  if (p.t === 'bool')
    return html`<input type="checkbox" checked=${p.v === '1'} onChange=${(e: Event) =>
      onParam(row.name, p.n, (e.target as HTMLInputElement).checked ? '1' : '0')} />`;
  if (p.t === 'enum')
    return html`<select onChange=${(e: Event) =>
      onParam(row.name, p.n, (e.target as HTMLSelectElement).value)}>
      ${(p.opts || []).map((o) => html`<option selected=${o === p.v}>${o}</option>`)}</select>`;
  return html`<input type="text" value=${p.v}
    onChange=${(e: Event) => onParam(row.name, p.n, (e.target as HTMLInputElement).value)} />`;
}

// Bus-placement conflicts among the enabled devices: two sharing an I/O-page
// address, or a non-zero interrupt vector, collide on the bus. Advisory (the
// backend refuses an overlapping registration authoritatively); this flags it
// in the editor as it is configured. Returns a reason per conflicting device.
function computeConflicts(rows: Row[]): Record<string, string> {
  const pv = (r: Row, n: string) => {
    const p = r.params.find((q) => q.n === n);
    return p ? p.v : '';
  };
  const bump = (m: Map<string, string[]>, key: string, name: string) => {
    const a = m.get(key);
    if (a) a.push(name);
    else m.set(key, [name]);
  };
  const addr = new Map<string, string[]>();
  const vec = new Map<string, string[]>();
  rows.forEach((r) => {
    const a = pv(r, 'base_addr');
    if (a) bump(addr, a, r.name);
    const v = pv(r, 'intr_vector');
    if (v && !/^0+$/.test(v)) bump(vec, v, r.name);
  });
  const out: Record<string, string> = {};
  const flag = (m: Map<string, string[]>, kind: string) => {
    m.forEach((names) => {
      if (names.length < 2) return;
      names.forEach((n) => {
        const others = names.filter((x) => x !== n).join(', ');
        const msg = kind + ' shared with ' + others;
        out[n] = out[n] ? out[n] + '; ' + msg : msg;
      });
    });
  };
  flag(addr, 'address');
  flag(vec, 'vector');
  return out;
}

function DevRow({
  row,
  cfg,
  device,
  sub,
  conflict,
  onToggle,
  onParam,
  onImage,
}: {
  row: Row;
  cfg: string;
  device: string;
  sub: boolean; // a controller's drive: keep its own enable toggle
  conflict?: string;
  onToggle: SetEnabled;
  onParam: SetParam;
  onImage: SetImage;
}) {
  const loc = useLocation();
  const open = device === row.name;
  // serial lines are edited as one field each, so their raw role/host/port
  // parameters are lifted out of the generic parameter grid
  const lines = serialLines(row.params);
  const lineParams = new Set(lines.flatMap((l) => [l.roleParam, l.hostParam, l.portParam]));
  const gridParams = row.params.filter((p) => p.n !== 'image' && !lineParams.has(p.n));
  const hasDetail = gridParams.length > 0 || lines.length > 0;
  const toggleOpen = () => {
    const base = '/config/' + encodeURIComponent(cfg);
    const path = open ? base : base + '/' + encodeURIComponent(row.name);
    loc.route(path + location.search);
  };
  return html`<div class=${'cfg-dev' + (row.enabled ? '' : ' off')}>
    <div class="dev-sub">
      <span class="dev-label">${row.label}</span>
      <span class="dev-handle mono">${row.name}</span>
      <span class="muted" style="font-size:var(--fs-0)">${row.type}</span>
      ${conflict ? html`<span class="chip err" title=${conflict}>⚠ conflict</span>` : null}
      ${
        row.takesImage
          ? html`<${ImageField} drive=${row.name} image=${row.image}
            onPick=${(n: string) => onImage(row.name, n)} />`
          : null
      }
      <span class="spacer"></span>
      ${
        // a controller's drive is enabled/disabled in place; a top-level device
        // is instead removed, since the list only carries added devices
        sub
          ? html`<label class="dev-enable"><input type="checkbox" checked=${row.enabled}
              onChange=${(e: Event) => onToggle(row.name, (e.target as HTMLInputElement).checked)} /> enabled</label>`
          : null
      }
      ${
        hasDetail
          ? html`<button class="btn small" onClick=${toggleOpen}>${open ? 'Hide' : 'Parameters'}</button>`
          : null
      }
      ${
        sub
          ? null
          : html`<${DelButton} label="Remove" confirmLabel="Confirm remove"
              onConfirm=${() => onToggle(row.name, false)} />`
      }
    </div>
    ${
      open && hasDetail
        ? html`<div class="params">
      ${
        lines.length
          ? html`<div class="serial-lines">
          <div class="serial-lines-head">Serial lines — a bare port listens, <span class="mono">host:port</span> connects, <span class="mono">ws</span> is a browser terminal</div>
          ${lines.map(
            (l) => html`<${SerialPortField} row=${row} line=${l} onParam=${onParam} key=${l.roleParam} />`
          )}
        </div>`
          : null
      }
      ${
        gridParams.length
          ? html`<div class="p-grid">
        ${gridParams.map(
          (p) => html`<div class="p-name">${p.n}${
            p.s && p.s !== p.n ? html` <span class="p-short">(${p.s})</span>` : null
          }</div>
          <div class="p-val">${paramControl(row, p, onParam)}${
            p.u ? html`<span class="unit">${p.u}</span>` : null
          }</div>
          <div class="p-info">${p.i}</div>`
        )}
      </div>`
          : null
      }
    </div>`
        : null
    }
    ${
      row.enabled
        ? (row.drives || []).map(
            (d) => html`<${DevRow} row=${d} cfg=${cfg} device=${device} sub=${true}
              onToggle=${onToggle} onParam=${onParam} onImage=${onImage} key=${d.name} />`
          )
        : null
    }
  </div>`;
}

// The configuration's operator-friendly title, edited in place. It commits on
// blur or Enter and writes through to the file (the running machine is
// untouched); the technical name stays the identity used to rename and address
// the configuration.
function TitleField({ name, title }: { name: string; title: string }) {
  const [draft, setDraft] = useState(title);
  useEffect(() => setDraft(title), [title]);
  const commit = () => {
    const v = draft.trim();
    if (v !== title) setConfigTitle(name, v);
  };
  return html`<input class="cfg-title-input" type="text" value=${draft}
    aria-label="configuration title"
    onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
    onChange=${commit} onBlur=${commit}
    onKeyDown=${(e: KeyboardEvent) => {
      if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
    }} />`;
}

// Binds the configuration to a DIP-switch setting: the board loads it at
// power-on when the four switches read that value. A value another
// configuration already claims is shown disabled, since at most one may hold it.
// The live switch reading is shown so the operator can see which configuration
// the board would bring up now.
function DipField({ name, dip }: { name: string; dip: number }) {
  const s = useStore();
  const liveDip = s.hw.dip.reduce((v, b, i) => v | (b ? 1 << i : 0), 0);
  const holderOf = (v: number) =>
    (s.configs || []).find((c) => c.name !== name && (c.dip_value ?? -1) === v);
  const onPick = (e: Event) => {
    const v = parseInt((e.target as HTMLSelectElement).value, 10);
    setConfigDip(name, v < 0 ? null : v);
  };
  return html`<label class="cfg-dip">
    <span class="muted">Power-on DIP</span>
    <select onChange=${onPick}>
      <option value="-1" selected=${dip < 0}>none</option>
      ${Array.from({ length: 16 }, (_, v) => {
        const h = holderOf(v);
        return html`<option value=${v} selected=${dip === v} disabled=${!!h}>${
          v + (h ? ' — ' + (h.title || h.name) : '')
        }</option>`;
      })}
    </select>
    <span class="cfg-dip-live muted">switches read ${liveDip}${
      liveDip === dip && dip >= 0 ? ' — this one' : ''
    }</span>
  </label>`;
}

function Detail({ name }: { name: string }) {
  const s = useStore();
  const loc = useLocation();
  const { params } = useRoute();
  const device = params.device ? decodeURIComponent(params.device) : '';
  const isCurrent = name === s.configCurrent;
  const summary = (s.configs || []).find((c) => c.name === name);
  const title = summary?.title || name;
  const dip = summary?.dip_value ?? -1;

  const [staged, setStaged] = useState<Staged | null>(null);
  const [dirty, setDirty] = useState(false);
  useEffect(() => {
    if (isCurrent) {
      setStaged(null);
      setDirty(false);
      return;
    }
    let live = true;
    fetchConfigSnapshot(name).then((doc) => {
      if (live) {
        setStaged(seedStaged(doc || { devices: [] }));
        setDirty(false);
      }
    });
    return () => {
      live = false;
    };
  }, [name, isCurrent]);

  // Warn before leaving with staged edits that Save has not written. The guard
  // covers in-app navigation (the sidebar, the configuration list); beforeunload
  // covers a reload or tab close, where only the browser's own prompt is
  // available. Current-configuration edits reach the machine live and are never
  // staged, so they need no guard.
  useEffect(() => {
    if (isCurrent || !dirty) return;
    const g = () =>
      confirmModal(
        'Discard unsaved changes?',
        'This configuration has parameter changes that have not been saved. Leave and discard them?',
        'Discard'
      );
    setNavGuard(g);
    const onBeforeUnload = (e: BeforeUnloadEvent) => {
      e.preventDefault();
      e.returnValue = '';
    };
    window.addEventListener('beforeunload', onBeforeUnload);
    return () => {
      clearNavGuard(g);
      window.removeEventListener('beforeunload', onBeforeUnload);
    };
  }, [isCurrent, dirty]);

  // live edits reach the running machine at once; staged edits sit in the editor
  const liveToggle: SetEnabled = (dev, on) =>
    liveSetParam(dev, 'enabled', on ? '1' : '0', on ? 'device enabled' : 'device disabled');
  const liveParam: SetParam = (dev, pn, val) => liveSetParam(dev, pn, val, 'parameter set');
  const liveImage: SetImage = (dev, img) => {
    liveSetParam(dev, 'image', img, img ? 'image attached' : 'image detached');
    setTimeout(() => refreshDevices().catch(() => {}), 300);
  };
  const stage = (fn: (st: Staged) => void) => {
    setStaged((prev) => {
      const base = prev || { enabled: {}, params: {} };
      const st: Staged = { enabled: { ...base.enabled }, params: { ...base.params } };
      fn(st);
      return st;
    });
    setDirty(true);
  };
  const stagedToggle: SetEnabled = (dev, on) => stage((st) => (st.enabled[dev] = on));
  const stagedParam: SetParam = (dev, pn, val) =>
    stage((st) => (st.params[dev] = { ...(st.params[dev] || {}), [pn]: val }));
  const stagedImage: SetImage = (dev, img) => stagedParam(dev, 'image', img);

  const roots = isCurrent
    ? s.devmodel.map(liveRow)
    : staged
    ? s.devmodel.map((d) => storedRow(d, staged))
    : [];
  // the list carries only the devices this configuration has added; the rest are
  // offered by the Add CTA. The registry pre-instantiates several controllers of
  // some types (four DZV11s, two DL11s) at fixed addresses; the picker offers
  // each type once and adds the next free instance, its address then set from the
  // parameter dropdown.
  const visible = roots.filter((r) => r.enabled);
  const available = (() => {
    const byType = new Map<string, { name: string; label: string; type: string; count: number }>();
    roots
      .filter((r) => !r.enabled)
      .forEach((r) => {
        const g = byType.get(r.type);
        if (g) {
          g.count++;
          return;
        }
        // the picker offers the type and adds its next free instance, so drop
        // the trailing number that only tells one instance from another
        byType.set(r.type, { name: r.name, label: r.label.replace(/ \d+$/, ''), type: r.type, count: 1 });
      });
    return Array.from(byType.values()).map((g) => ({
      name: g.name,
      label: g.count > 1 ? g.label + ' · ' + g.count + ' available' : g.label,
      type: g.type,
    }));
  })();
  const conflicts = computeConflicts(visible);
  const doAdd = async () => {
    const pick = await pickDevice('Add a device', available);
    if (pick) (isCurrent ? liveToggle : stagedToggle)(pick, true);
  };

  const doSaveAs = async () => {
    const nn = await promptModal('Save configuration as', 'Name', name, 'Save');
    if (!nn) return;
    if (await saveConfigFromLive(nn)) loc.route('/config/' + encodeURIComponent(nn));
  };
  const doRevert = async () => {
    if (
      !(await confirmModal(
        'Revert to the saved ' + esc(name) + '?',
        'This re-initialises the running machine to the saved device set, dropping any change made since the last save.',
        'Revert'
      ))
    )
      return;
    await applyConfig(name);
    await powerDownAfterLoad();
    loc.route('/dashboard');
  };
  const doSaveStored = async () => {
    if (staged && (await saveConfigDoc(name, serialize(staged)))) setDirty(false);
  };
  // A configuration is a machine that has been built and not yet switched on:
  // loading one leaves it powered down, so the operator decides when it starts
  // rather than finding it already running something.
  const powerDownAfterLoad = () => liveControl('dc_off', 'machine off — switch it on when ready');

  const doLoad = async () => {
    if (
      s.hw.powered !== false &&
      !(await confirmModal(
        'Load while the machine is on?',
        'The machine is powered up. Loading <b>' +
          esc(name) +
          '</b> reconfigures every device and switches it off — the running system will not survive it.',
        'Load anyway'
      ))
    )
      return;
    // The loaded configuration is the machine now, so the dashboard — the
    // screen that shows that machine — is where the operator wants to land.
    await applyConfig(name);
    await powerDownAfterLoad();
    loc.route('/dashboard');
  };
  const doRename = async () => {
    const nn = await promptModal('Rename configuration', 'New name', name, 'Rename');
    if (!nn || nn === name) return;
    if (await renameConfig(name, nn)) loc.route('/config/' + encodeURIComponent(nn));
  };
  const doDelete = async () => {
    if (await deleteConfig(name)) loc.route('/config');
  };

  return html`<div class="cfg-detail"><div class="card">
    <div class="cfg-detail-head">
      <div class="cfg-heading">
        <div class="cfg-title">
          <${TitleField} name=${name} title=${title} key=${name} />
          ${isCurrent ? html`<${Chip} cls="ok">current</${Chip}>` : null}
          ${isCurrent && s.configModified ? html`<${Chip} cls="warn">modified</${Chip}>` : null}
          ${dip >= 0 ? html`<${Chip} cls="out">DIP ${dip}</${Chip}>` : null}
        </div>
        <div class="cfg-subid mono">${name}</div>
      </div>
      <div class="cfg-actions">
        ${
          isCurrent
            ? html`<button class="btn small primary" onClick=${() => saveConfigFromLive(name)}>Save</button>
              <button class="btn small" onClick=${doSaveAs}>Save As…</button>
              <button class="btn small" onClick=${doRevert}>Revert</button>`
            : html`<button class="btn small primary" disabled=${!dirty} onClick=${doSaveStored}>Save</button>
              <button class="btn small" onClick=${doLoad}>Load</button>`
        }
        <button class="btn small" onClick=${doRename}>Rename…</button>
        <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${doDelete} />
      </div>
    </div>
    <div class="cfg-detail-sub">
      <${DipField} name=${name} dip=${dip} />
      <span class="spacer"></span>
      <button class="btn small primary" onClick=${doAdd}>+ Add device</button>
    </div>
    <div class="cfg-editor">
      ${
        visible.length
          ? visible.map(
              (r) => html`<${DevRow} row=${r} cfg=${name} device=${device} sub=${false}
                conflict=${conflicts[r.name]}
                onToggle=${isCurrent ? liveToggle : stagedToggle}
                onParam=${isCurrent ? liveParam : stagedParam}
                onImage=${isCurrent ? liveImage : stagedImage} key=${r.name} />`
            )
          : html`<div class="cfg-empty muted">No devices yet — use “Add device” to add one.</div>`
      }
    </div>
  </div></div>`;
}

function MasterRow({ c }: { c: ConfigSummary }) {
  const s = useStore();
  const loc = useLocation();
  const { params } = useRoute();
  const selected = params.name ? decodeURIComponent(params.name) : '';
  const isCurrent = c.name === s.configCurrent;
  const dip = c.dip_value ?? -1;
  return html`<button class=${'cfg-item' + (c.name === selected ? ' active' : '')}
    onClick=${() => {
      if (c.name !== selected) guardedRoute(loc, '/config/' + encodeURIComponent(c.name));
    }}>
    <span class="cfg-item-top">
      <span class="cfg-item-name">${c.title || c.name}</span>
      <span class="cfg-item-marks">
        ${isCurrent ? html`<${Chip} cls="ok">current</${Chip}>` : null}
        ${isCurrent && s.configModified ? html`<${Chip} cls="warn">modified</${Chip}>` : null}
        ${dip >= 0 ? html`<${Chip} cls="out">DIP ${dip}</${Chip}>` : null}
      </span>
    </span>
    ${c.title && c.title !== c.name
      ? html`<span class="cfg-item-sub mono">${c.name}</span>`
      : null}
    <span class="cfg-item-devs muted mono">${(c.enabled || []).join(' · ') || 'all devices off'}</span>
  </button>`;
}

export function ConfigsPage() {
  const s = useStore();
  const loc = useLocation();
  const { params } = useRoute();
  const name = params.name ? decodeURIComponent(params.name) : '';
  useEffect(() => {
    loadConfigs().catch(() => {});
  }, []);
  const configs = s.configs || [];
  // land on the running configuration rather than an empty detail
  useEffect(() => {
    if (!name && s.configCurrent && configs.some((c) => c.name === s.configCurrent))
      loc.route('/config/' + encodeURIComponent(s.configCurrent), true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [name, s.configCurrent, configs.length]);
  const exists = configs.some((c) => c.name === name);
  return html`<section class="page active" data-page="configurations">
    <div class="cfg-layout">
      <div class="cfg-master card">
        <div class="card-head"><h3>Configurations</h3></div>
        <div class="cfg-list">
          ${
            configs.length
              ? configs.map((c) => html`<${MasterRow} c=${c} key=${c.name} />`)
              : html`<div class="muted" style="padding:8px 14px; font-size:var(--fs-1)">${
                  s.configs == null ? 'Loading…' : 'No saved configurations yet.'
                }</div>`
          }
        </div>
      </div>
      ${
        !name
          ? html`<div class="cfg-detail"><div class="card"><div class="cfg-empty muted">
              Select a configuration to view its devices and image assignments.</div></div></div>`
          : exists
          ? html`<${Detail} name=${name} key=${name} />`
          : html`<div class="cfg-detail"><div class="card"><div class="cfg-empty muted">
              No configuration named “${name}”.</div></div></div>`
      }
    </div>
  </section>`;
}
