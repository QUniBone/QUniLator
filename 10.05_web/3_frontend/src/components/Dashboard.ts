import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import { useLocation } from 'preact-iso';
import { useStore, store, emit } from '../store';
import { liveControl, loadConfigs, fetchConfigSnapshot, setConfigLayout } from '../api';
import { initLiveTerminal, teardownTerminals } from '../lib/terminals';
import { enabledDevices } from '../lib/devmodel';
import { placeItems, gridRows, fits, occupancyExcept } from '../lib/dashlayout';
import type { GridItem } from '../lib/dashlayout';
import { toast } from '../lib/toast';
import { Led, Chip } from './common';
import { widgetFor, widgetCells, DeviceWidget } from './widgets';
import type { LiveDev, DashLayout } from '../types';

// The running configuration's name and title, with a cog that jumps straight to
// its configuration screen.
function DashHeader() {
  const s = useStore();
  const loc = useLocation();
  const name = s.configCurrent;
  const title = (s.configs || []).find((c) => c.name === name)?.title || name;
  return html`<div class="dash-head">
    <div class="dash-cfg">
      <span class="dash-cfg-title">${title || 'no configuration'}</span>
      ${title && title !== name ? html`<span class="dash-cfg-name mono">${name}</span>` : null}
      ${s.configModified ? html`<${Chip} cls="warn">modified</${Chip}>` : null}
    </div>
    <button class="btn small dash-cog" title="Configuration"
      onClick=${() => loc.route('/config' + (name ? '/' + encodeURIComponent(name) : ''))}>⚙</button>
  </div>`;
}

// One switch of the 11/03 bezel: a bat-handle toggle with a silkscreen legend
// above (two-position) and/or below it. `momentary` springs back and fires on
// click; `two` reflects and sets a position.
function PanelSwitch({
  kind,
  label,
  pos,
  posLabels,
  disabled,
  onFire,
  onToggle,
}: {
  kind: 'momentary' | 'two';
  label: string;
  pos?: 'top' | 'bottom';
  posLabels?: [string, string];
  disabled?: boolean;
  onFire?: () => void;
  onToggle?: () => void;
}) {
  const click = () => {
    if (disabled) return;
    if (kind === 'momentary') onFire?.();
    else onToggle?.();
  };
  return html`<div class=${'cp-ctrl cp-sw ' + kind + (disabled ? ' off' : '')} data-pos=${pos || 'mid'}>
    <span class="cp-el"><button class="cp-toggle" type="button" disabled=${!!disabled}
      role=${kind === 'two' ? 'switch' : undefined}
      aria-checked=${kind === 'two' ? pos === 'top' : undefined}
      aria-label=${posLabels ? label + ' ' + posLabels[0] + '/' + posLabels[1] : label}
      onClick=${click}><span class="cp-bat"></span></button></span>
    <span class="cp-leg">${label}${
      posLabels
        ? html`<span class="cp-pos"><span>${posLabels[0]}</span><span>${posLabels[1]}</span></span>`
        : null
    }</span>
  </div>`;
}

// The PDP-11/03 control bezel: PWR OK + RUN lamps and the RESTART /
// HALT-ENABLE / AUX-ON-OFF switches, the single run-state and power controls.
// AUX ON/OFF is the auxiliary DC power switch, driving the dc_on/dc_off flag.
function ControlPanel() {
  const s = useStore();
  const powered = s.hw.powered !== false;
  // HALT/ENABLE holds its last definite position: keep the last known halt
  // reading and only move the switch when a new definite value arrives, so a
  // transitional gap does not flicker it.
  const lastHalt = useRef(s.bus.halted);
  if (typeof s.bus.halted === 'boolean') lastHalt.current = s.bus.halted;
  const halted = lastHalt.current;
  const run = powered && !halted;

  const restart = () => liveControl('restart', 'reset and restart from boot');
  const setHalt = () => {
    if (halted) {
      liveControl('continue', 'HALT released — CPU running');
      store.bus.halted = false;
    } else {
      liveControl('halt', 'HALT asserted — CPU stopped');
      store.bus.halted = true;
    }
    emit();
  };
  // power is authoritative from the backend's `powered` state event; do not
  // guess it optimistically, so a machine that has not yet learned dc_on/dc_off
  // does not strand the UI in a frozen "off" it never actually entered
  const setPower = () =>
    powered
      ? liveControl('dc_off', 'DC off — machine powered down')
      : liveControl('dc_on', 'DC on — machine powered up');

  return html`<div class="card cp-card">
    <div class="card-head"><h3>Control panel</h3></div>
    <div class="cp-body">
      <div class="cp-lamps">
        <div class="cp-lampbezel">
          <span class="cp-lampcell">${html`<${Led} on=${powered} title="PWR OK" />`}</span>
          <span class="cp-lampcell">${html`<${Led} on=${run} title="RUN" />`}</span>
        </div>
        <div class="cp-lamplegs"><span class="cp-lampcell">PWR OK</span><span class="cp-lampcell">RUN</span></div>
      </div>
      <img class="cp-logo" src="/digital-logo.svg" alt="digital" width="72" height="21" />
      <div class="cp-switches">
        ${html`<${PanelSwitch} kind="momentary" label="RESTART" disabled=${!powered} onFire=${restart} />`}
        ${html`<${PanelSwitch} kind="two" label="HALT"
          pos=${halted ? 'bottom' : 'top'} disabled=${!powered} onToggle=${setHalt} />`}
        ${html`<${PanelSwitch} kind="two" label="AUX" posLabels=${['ON', 'OFF']}
          pos=${powered ? 'top' : 'bottom'} onToggle=${setPower} />`}
      </div>
    </div>
  </div>`;
}

// The cape's own front panel: activity LEDs and DIP switches, display-only, in
// the shared LED visual language. Dark while the machine is powered off.
function FrontPanel() {
  const s = useStore();
  const powered = s.hw.powered !== false;
  return html`<div class="card frontpanel"><div class="card-head"><h3>Front panel</h3></div>
    <div class="card-body">
      <div class="fp-block">
        <div class="fp-cells fp-cells-led">${s.hw.leds.map(
          (v, i) => html`<div class="fp-cell">${html`<${Led} on=${powered && v} title=${'Activity ' + i} />`}
            <span class="fp-n">${i}</span></div>`
        )}</div></div>
      <div class="fp-block">
        <div class="fp-cells fp-cells-sw">${s.hw.dip.map(
          (v, i) => html`<div class="fp-cell"><span class=${'dip' + (v ? ' on' : '')}></span>
            <span class="fp-n">${i + 1}</span></div>`
        )}</div></div>
    </div></div>`;
}

export function TerminalHost() {
  const host = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    if (host.current) initLiveTerminal(host.current);
    return () => teardownTerminals();
  }, []);
  return html`<div class="term" ref=${host} tabindex="0" aria-label="VT100 terminal, 80 columns by 24 rows"></div>`;
}

// The live console as a first-class card, so it reads like the other cards on
// the dashboard.
function ConsoleCard() {
  return html`<div class="card console-card">
    <div class="card-head"><h3>Console</h3></div>
    <div class="card-body dash-term">${html`<${TerminalHost} />`}</div>
  </div>`;
}

// ---- the arrangeable grid ----
const CELL = 44; // px per grid square
const GAP = 6;
// the fixed cards, their natural size in grid cells, and their titles
const FIXED = [
  { key: 'controlpanel', w: 13, h: 3, label: 'Control panel' },
  { key: 'frontpanel', w: 7, h: 3, label: 'Front panel' },
  { key: 'console', w: 15, h: 11, label: 'Console' },
];
const FIXED_LABEL: Record<string, string> = {
  controlpanel: 'Control panel',
  frontpanel: 'Front panel',
  console: 'Console',
};
// a hidden card shows compact in edit mode: just enough cells for its name row
const HIDDEN_CELLS = { w: 6, h: 2 };

function renderCard(key: string, devByName: Record<string, LiveDev>) {
  if (key === 'controlpanel') return html`<${ControlPanel} />`;
  if (key === 'frontpanel') return html`<${FrontPanel} />`;
  if (key === 'console') return html`<${ConsoleCard} />`;
  const d = devByName[key];
  return d ? html`<${DeviceWidget} d=${d} />` : null;
}

// A hidden widget in edit mode: represented by just its title, handle and a
// line of basic configuration, so the operator can find and re-show it without
// it taking a full widget's space.
function renderHidden(key: string, devByName: Record<string, LiveDev>) {
  const d = devByName[key];
  const title = d ? d.label || d.name : FIXED_LABEL[key] || key;
  const info = d ? d.img || d.type : '';
  return html`<div class="card hidden-card">
    <div class="card-head"><h3>${title}</h3></div>
    <div class="card-body hidden-info">
      <span class="mono">${key}</span>${info ? html` · <span>${info}</span>` : null}
    </div>
  </div>`;
}

// The dashboard as a grid of square cells. Every card — the panels, the console
// and each device widget — is a block of whole cells placed on free space.
// An edit mode lets the operator drag cards and hide them (eye), then Save/Revert
// the arrangement, which is stored per configuration.
function DashGrid() {
  const s = useStore();
  const cfg = s.configCurrent;
  const gridRef = useRef<HTMLDivElement | null>(null);
  const [cols, setCols] = useState(12);
  const [edit, setEdit] = useState(false);
  const [layout, setLayout] = useState<DashLayout>({});
  const [dirty, setDirty] = useState(false);
  const drag = useRef<{ key: string; dx: number; dy: number } | null>(null);
  const [preview, setPreview] = useState<{ key: string; x: number; y: number; ok: boolean } | null>(null);

  // load the stored layout for the running configuration
  useEffect(() => {
    let live = true;
    if (cfg)
      fetchConfigSnapshot(cfg).then((doc) => {
        if (live) {
          setLayout((doc?.layout as DashLayout) || {});
          setDirty(false);
        }
      });
    else setLayout({});
    return () => {
      live = false;
    };
  }, [cfg]);

  // columns follow the container width; the cell size is fixed
  useEffect(() => {
    const el = gridRef.current;
    if (!el) return;
    const measure = () => setCols(Math.max(6, Math.floor((el.clientWidth + GAP) / (CELL + GAP))));
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const devs = enabledDevices().filter((d) => widgetFor(d));
  const devByName: Record<string, LiveDev> = {};
  devs.forEach((d) => (devByName[d.name] = d));
  const isHidden = (key: string) => !!layout[key]?.hidden;

  // fixed cards + device widgets; a hidden card shrinks to a compact tile in
  // edit mode, and widths are clamped so a card never exceeds the grid
  const sized = (key: string, nat: { w: number; h: number }): GridItem => {
    const c = edit && isHidden(key) ? HIDDEN_CELLS : nat;
    return { key, w: Math.min(c.w, cols), h: c.h };
  };
  const allItems: GridItem[] = [
    ...FIXED.map((f) => sized(f.key, f)),
    ...devs.map((d) => sized(d.name, widgetCells(d))),
  ];
  const items = allItems.filter((it) => edit || !isHidden(it.key));
  // Pack against a width that honours any stored position past the visible edge,
  // so a card the operator pushed off the right keeps its place instead of
  // reflowing back in.
  const storedExtent = items.reduce((m, it) => {
    const p = layout[it.key];
    return p && Number.isFinite(p.x) ? Math.max(m, p.x + it.w) : m;
  }, 0);
  const placed = placeItems(items, layout, Math.max(cols, storedExtent));
  // The canvas grows to hold its content, plus the live drag preview, so moving
  // a card past the right or bottom edge extends the grid rather than pinning it.
  const dragItem = preview ? placed.find((p) => p.key === preview.key) : null;
  const extentCols = Math.max(
    cols,
    placed.reduce((m, p) => Math.max(m, p.x + p.w), 0),
    dragItem ? preview!.x + dragItem.w : 0
  );
  const rows = Math.max(gridRows(placed), dragItem ? preview!.y + dragItem.h : 0);

  const patch = (key: string, p: Partial<DashLayout[string]>) => {
    setLayout((L) => {
      const cur = L[key] || { x: 0, y: 0 };
      return { ...L, [key]: { ...cur, ...p } };
    });
    setDirty(true);
  };

  // The drag reads the current placement/columns and the live preview through
  // refs so the window-level move/up listeners (attached once) always see fresh
  // values without being re-bound every render.
  const placedRef = useRef(placed);
  placedRef.current = placed;
  const colsRef = useRef(cols);
  colsRef.current = cols;
  const previewRef = useRef(preview);
  previewRef.current = preview;

  // While editing, every card holds an explicit position, so hiding or showing
  // one never reflows the others into the freed space. Freeze the current
  // placement into the layout the moment edit mode opens.
  useEffect(() => {
    if (!edit) return;
    setLayout((L) => {
      const next = { ...L };
      let changed = false;
      for (const p of placedRef.current) {
        const cur = L[p.key];
        if (!cur || cur.x !== p.x || cur.y !== p.y) {
          next[p.key] = { ...(cur || {}), x: p.x, y: p.y };
          changed = true;
        }
      }
      return changed ? next : L;
    });
  }, [edit]);

  useEffect(() => {
    const move = (e: MouseEvent) => {
      const d = drag.current;
      const el = gridRef.current;
      if (!d || !el) return;
      const it = placedRef.current.find((q) => q.key === d.key);
      if (!it) return;
      const r = el.getBoundingClientRect();
      let x = Math.round((e.clientX - r.left - d.dx) / (CELL + GAP));
      let y = Math.round((e.clientY - r.top - d.dy) / (CELL + GAP));
      x = Math.max(0, x);
      y = Math.max(0, y);
      // No right wall: bound the fit check to the card's own reach so only true
      // overlaps are rejected, letting the operator drag a card off the edge to
      // grow the canvas.
      const boundCols = Math.max(colsRef.current, x + it.w);
      const ok = fits(occupancyExcept(placedRef.current, d.key), x, y, it.w, it.h, boundCols);
      setPreview({ key: d.key, x, y, ok });
    };
    const up = () => {
      const d = drag.current;
      if (!d) return;
      drag.current = null;
      const pv = previewRef.current;
      if (pv && pv.ok) patch(d.key, { x: pv.x, y: pv.y });
      setPreview(null);
    };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
    return () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const onDown = (key: string) => (e: MouseEvent) => {
    if (!edit) return;
    e.preventDefault();
    const p = placedRef.current.find((q) => q.key === key);
    const el = gridRef.current;
    if (!p || !el) return;
    const r = el.getBoundingClientRect();
    drag.current = {
      key,
      dx: e.clientX - r.left - p.x * (CELL + GAP),
      dy: e.clientY - r.top - p.y * (CELL + GAP),
    };
    setPreview({ key, x: p.x, y: p.y, ok: true });
  };

  const save = async () => {
    if (await setConfigLayout(cfg, layout)) {
      setDirty(false);
      setEdit(false);
      toast('dashboard', 'layout saved');
    }
  };
  const revert = () => {
    setEdit(false);
    if (cfg)
      fetchConfigSnapshot(cfg).then((doc) => {
        setLayout((doc?.layout as DashLayout) || {});
        setDirty(false);
      });
    else setLayout({});
  };

  return html`<div class="dash-toolbar">
      ${
        edit
          ? html`<button class="btn small primary" disabled=${!dirty} onClick=${save}>Save layout</button>
              <button class="btn small" onClick=${revert}>Revert</button>
              <span class="muted" style="font-size:var(--fs-1)">drag a card to move it; the eye hides it</span>`
          : html`<button class="btn small" onClick=${() => setEdit(true)}>Edit layout</button>`
      }
    </div>
    <div class=${'dash-grid' + (edit ? ' editing' : '')} ref=${gridRef}
      style=${'grid-template-columns:repeat(' + extentCols + ',' + CELL + 'px);grid-auto-columns:' +
        CELL + 'px;grid-auto-rows:' + CELL + 'px;gap:' + GAP + 'px;' +
        (edit ? 'min-height:' + (rows + 2) * (CELL + GAP) + 'px;' : '')}>
      ${placed.map((p) => {
        const pv = preview && preview.key === p.key ? preview : null;
        const x = pv ? pv.x : p.x;
        const y = pv ? pv.y : p.y;
        const hidden = isHidden(p.key);
        return html`<div
          class=${'grid-item' + (hidden ? ' hidden' : '') + (pv ? ' dragging' + (pv.ok ? '' : ' bad') : '')}
          key=${p.key}
          style=${'grid-column:' + (x + 1) + '/span ' + p.w + ';grid-row:' + (y + 1) + '/span ' + p.h + ';'}>
          ${edit && hidden ? renderHidden(p.key, devByName) : renderCard(p.key, devByName)}
          ${
            edit
              ? html`<div class="gi-edit" onMouseDown=${onDown(p.key)}>
                  <span class="gi-move mono">⠿ ${p.key}</span>
                  <button class="gi-eye" title=${hidden ? 'show' : 'hide'}
                    onMouseDown=${(e: Event) => e.stopPropagation()}
                    onClick=${() => patch(p.key, { hidden: !hidden })}>${hidden ? '🙈' : '👁'}</button>
                </div>`
              : null
          }
        </div>`;
      })}
    </div>`;
}

export function Dashboard() {
  useStore();
  useEffect(() => {
    loadConfigs().catch(() => {});
  }, []);
  return html`<section class="page active" data-page="dashboard">
    ${html`<${DashHeader} />`}
    ${html`<${DashGrid} />`}
  </section>`;
}
