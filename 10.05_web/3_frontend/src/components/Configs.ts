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
import { esc } from '../lib/util';
import { confirmModal, promptModal, pickDevice } from '../lib/modals';
import {
  loadConfigs,
  refreshDevices,
  liveSetParam,
  fetchConfigSnapshot,
  saveConfigFromLive,
  saveConfigDoc,
  applyConfig,
  renameConfig,
  setDefaultConfig,
  deleteConfig,
} from '../api';
import { flatDevices } from '../lib/devmodel';
import { serialEndpoint, serialLines } from '../lib/serial';
import type { SerialLine, SerialRole } from '../lib/serial';
import { useStore } from '../store';
import { Toggle, Chip, ImageField, DelButton } from './common';
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
// the backend fills them in).
function serialize(st: Staged): ConfigSnapshot {
  const devices = flatDevices()
    .filter((d) => st.enabled[d.name])
    .map((d) => ({ name: d.name, enabled: true, params: st.params[d.name] || {} }));
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
}
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
    params: d.params
      .filter((p) => !p.ro && p.n !== 'image')
      .map((p) => ({ ...p, v: p.n in pv ? pv[p.n] : p.v })),
    drives: (d.drives || []).map((c) => storedRow(c, st)),
  };
}

type SetEnabled = (name: string, on: boolean) => void;
type SetParam = (name: string, param: string, value: string) => void;
type SetImage = (name: string, image: string) => void;

// A plain-language read-back of what a serial field's text does, translated by
// the same pure endpoint parser the field writes with.
function endpointHint(text: string): string {
  const ep = serialEndpoint.parse(text);
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
    <input class="serial-line-input mono" type="text" value=${draft} placeholder="port or host:port"
      disabled=${ro}
      onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
      onChange=${commit} onBlur=${commit} />
    <span class="serial-line-hint muted">${endpointHint(draft)}</span>
  </div>`;
}

function paramControl(row: Row, p: LiveParam, onParam: SetParam) {
  if (p.ro) return html`<span class="ro">${p.v}</span>`;
  if (p.t === 'enum')
    return html`<select onChange=${(e: Event) =>
      onParam(row.name, p.n, (e.target as HTMLSelectElement).value)}>
      ${(p.opts || []).map((o) => html`<option selected=${o === p.v}>${o}</option>`)}</select>`;
  return html`<input type="text" value=${p.v}
    onChange=${(e: Event) => onParam(row.name, p.n, (e.target as HTMLInputElement).value)} />`;
}

function DevRow({
  row,
  cfg,
  device,
  sub,
  onToggle,
  onParam,
  onImage,
}: {
  row: Row;
  cfg: string;
  device: string;
  sub: boolean; // a controller's drive: keep its own enable toggle
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
          ? html`<${Chip} cls=${row.enabled ? 'ok' : 'off'}>${row.enabled ? 'enabled' : 'disabled'}</${Chip}>
            <${Toggle} checked=${row.enabled} onChange=${(on: boolean) => onToggle(row.name, on)} />`
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
          <div class="serial-lines-head">Serial lines — a bare port listens, <span class="mono">host:port</span> connects</div>
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

function Detail({ name }: { name: string }) {
  const s = useStore();
  const loc = useLocation();
  const { params } = useRoute();
  const device = params.device ? decodeURIComponent(params.device) : '';
  const isCurrent = name === s.configCurrent;
  const isDefault = name === s.configDefault;

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
  // offered by the Add CTA
  const visible = roots.filter((r) => r.enabled);
  const available = roots
    .filter((r) => !r.enabled)
    .map((r) => ({ name: r.name, label: r.label, type: r.type }));
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
    applyConfig(name);
  };
  const doSaveStored = async () => {
    if (staged && (await saveConfigDoc(name, serialize(staged)))) setDirty(false);
  };
  const doApply = async () => {
    if (
      !s.bus.halted &&
      !(await confirmModal(
        'Apply while the PDP-11 is running?',
        'The CPU is running. Applying <b>' +
          esc(name) +
          '</b> reconfigures every device — the running system will not survive it.',
        'Apply anyway'
      ))
    )
      return;
    applyConfig(name);
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
        <div class="cfg-kicker">${isCurrent ? 'Current · live' : 'Stored'}</div>
        <div class="cfg-title">
          <span class="mono">${name}</span>
          ${isCurrent && s.configModified ? html`<${Chip} cls="warn">modified</${Chip}>` : null}
          ${isDefault ? html`<${Chip} cls="out">default</${Chip}>` : null}
        </div>
      </div>
      <div class="cfg-actions">
        ${
          isCurrent
            ? html`<button class="btn small primary" onClick=${() => saveConfigFromLive(name)}>Save</button>
              <button class="btn small" onClick=${doSaveAs}>Save As…</button>
              <button class="btn small" onClick=${doRevert}>Revert</button>`
            : html`<button class="btn small primary" disabled=${!dirty} onClick=${doSaveStored}>Save</button>
              <button class="btn small" onClick=${doApply}>Apply</button>`
        }
        ${isDefault ? null : html`<button class="btn small" onClick=${() => setDefaultConfig(name)}>Set default</button>`}
        <button class="btn small" onClick=${doRename}>Rename…</button>
        <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${doDelete} />
      </div>
    </div>
    <div class="cfg-detail-sub">
      <span class="muted" style="font-size:var(--fs-1)">${
        isCurrent
          ? 'Edits act on the running machine immediately, so this configuration goes modified.'
          : 'Edits are staged here and reach nothing until you Save.'
      }</span>
      <span class="spacer"></span>
      <button class="btn small primary" onClick=${doAdd}>+ Add device</button>
    </div>
    <div class="cfg-editor">
      ${
        visible.length
          ? visible.map(
              (r) => html`<${DevRow} row=${r} cfg=${name} device=${device} sub=${false}
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
  const isDefault = c.name === s.configDefault;
  return html`<button class=${'cfg-item' + (c.name === selected ? ' active' : '')}
    onClick=${() => loc.route('/config/' + encodeURIComponent(c.name))}>
    <span class="cfg-item-top">
      <span class="cfg-item-name mono">${c.name}</span>
      <span class="cfg-item-marks">
        ${isCurrent ? html`<${Chip} cls="ok">current</${Chip}>` : null}
        ${isCurrent && s.configModified ? html`<${Chip} cls="warn">modified</${Chip}>` : null}
        ${isDefault ? html`<${Chip} cls="out">default</${Chip}>` : null}
      </span>
    </span>
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
