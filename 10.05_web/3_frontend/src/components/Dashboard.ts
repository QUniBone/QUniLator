import { html } from '../html';
import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { useLocation } from 'preact-iso';
import { useStore, store, emit } from '../store';
import { liveControl, loadConfigs, fetchConfigSnapshot, setConfigLayout } from '../api';
import { initLiveTerminal, teardownTerminals, consoleSource, focusConsole } from '../lib/terminals';
import { recordingState, startRecording, stopRecording } from '../api';
import { enabledDevices } from '../lib/devmodel';
import { placeItems, gridRows, fits, occupancyExcept } from '../lib/dashlayout';
import type { GridItem } from '../lib/dashlayout';
import { toast } from '../lib/toast';
import { Led, Chip } from './common';
import { widgetFor, widgetOptions, WidgetCard } from './widgets';
import type { Cells, WidgetOpts } from './widgets';
import type { LiveDev, DashLayout } from '../types';

// The running configuration's name and title, with a cog that jumps straight to
// its configuration screen.
function DashHeader({ tools }: { tools?: ComponentChildren }) {
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
    <span class="spacer"></span>
    ${tools}
    <button class="btn small dash-cog" title="Configuration"
      onClick=${() => loc.route('/config' + (name ? '/' + encodeURIComponent(name) : ''))}>⚙</button>
  </div>`;
}

// One switch of the 11/03 bezel: a bat-handle toggle with a silkscreen legend
// above (two-position) and/or below it. `momentary` springs back and fires on
// click; `two` reflects and sets a position. A momentary switch rests up, as
// the machine's does — resting centred read as a switch sitting off beside two
// that were up.
function PanelSwitch({
  kind,
  label,
  info,
  pos,
  posLabels,
  disabled,
  onFire,
  onToggle,
}: {
  kind: 'momentary' | 'two';
  label: string;
  info?: string;
  pos?: 'top' | 'bottom';
  posLabels?: [string, string];
  disabled?: boolean;
  onFire?: () => void;
  onToggle?: () => void;
}) {
  // A momentary switch is over before the eye catches it: the click releases in
  // a few milliseconds, so :active alone shows nothing and the operator cannot
  // tell the press landed. Firing throws the bat for a moment and lets it
  // spring back. The reset is a timer rather than the animation's end so the
  // throw is still shown where animation is off (prefers-reduced-motion).
  const [firing, setFiring] = useState(false);
  const throwTimer = useRef(0);
  useEffect(() => () => window.clearTimeout(throwTimer.current), []);
  const click = () => {
    if (disabled) return;
    if (kind === 'momentary') {
      setFiring(true);
      window.clearTimeout(throwTimer.current);
      throwTimer.current = window.setTimeout(() => setFiring(false), 320);
      onFire?.();
    } else onToggle?.();
  };
  return html`<div
    class=${'cp-ctrl cp-sw ' + kind + (disabled ? ' off' : '') + (firing ? ' firing' : '')}
    data-pos=${pos || 'top'}
    title=${info || null}>
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
//
// The legends are the console's, and two of them do not describe what they
// drive here: PWR OK is lit by the emulation being on the bus rather than by a
// supply rail, and AUX ON/OFF switches that emulation rather than any DC. The
// artwork is kept as it is - it is a reproduction of a real bezel - so the
// explanation goes in the hover text instead, one line per control saying what
// it actually does. The backplane's own power is reported apart, by the DCOK
// and POK indicators of the title bar, which read the bus lines themselves.
const CP_INFO = {
  pwr:
    'PWR OK — lit while QUniLator’s emulation is on the bus: the configuration’s ' +
    'cards are installed and answering their addresses. Despite the legend this is ' +
    'not a supply rail. The backplane’s own DC and AC power are the DCOK and POK ' +
    'indicators in the title bar, which read the bus lines.',
  run: 'RUN — lit while the processor is executing. Dark when it is halted, and dark while the emulation is off the bus.',
  restart:
    'RESTART — resets the machine and restarts it from its power-up vector. It ' +
    'releases HALT and runs a DCOK/POK sequence on the bus, which asserts BINIT; ' +
    'every emulated card is taken out and put back, dropping the state a card loses ' +
    'with its supply. Not a bare INIT: the processor comes up executing from the ' +
    'power-up vector rather than continuing where it was.',
  halt:
    'HALT — stops and starts the processor. The emulated cards stay on the bus ' +
    'either way: this is the processor’s run state, not the emulation’s.',
  aux:
    'AUX ON/OFF — switches QUniLator’s emulation on and off; it moves no real power ' +
    'and puts no edge on the bus’s power lines. OFF stops the processor and takes ' +
    'every emulated card out of the machine, so nothing of it answers an address and ' +
    'each drive gives up its medium — the board goes on carrying the configuration. ' +
    'ON puts the cards back, with the media they held, and starts the machine.',
};

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

  // The console is one plate let into the machine's front: the card surface is
  // the front, the plate carries the window, the logo and the switches.
  return html`<div class="card cp-card">
    <div class="card-head"><h3>Control panel</h3></div>
    <div class="card-body cp-body"><div class="cp-plate">
      <div class="cp-lamps">
        <div class="cp-lampbezel">
          <span class="cp-lampcell" title=${CP_INFO.pwr}
            >${html`<${Led} on=${powered} title="PWR OK" info=${CP_INFO.pwr} />`}</span>
          <span class="cp-lampcell" title=${CP_INFO.run}
            >${html`<${Led} on=${run} title="RUN" info=${CP_INFO.run} />`}</span>
        </div>
        <div class="cp-lamplegs"><span class="cp-lampcell" title=${CP_INFO.pwr}>PWR OK</span
          ><span class="cp-lampcell" title=${CP_INFO.run}>RUN</span></div>
      </div>
      <img class="cp-logo" src="/digital-logo.svg" alt="digital" width="72" height="21" />
      <div class="cp-switches">
        ${html`<${PanelSwitch} kind="momentary" label="RESTART" info=${CP_INFO.restart}
          disabled=${!powered} onFire=${restart} />`}
        ${html`<${PanelSwitch} kind="two" label="HALT" info=${CP_INFO.halt}
          pos=${halted ? 'bottom' : 'top'} disabled=${!powered} onToggle=${setHalt} />`}
        ${html`<${PanelSwitch} kind="two" label="AUX" posLabels=${['ON', 'OFF']} info=${CP_INFO.aux}
          pos=${powered ? 'top' : 'bottom'} onToggle=${setPower} />`}
      </div>
    </div></div>
  </div>`;
}

// The cape's own front panel: activity LEDs and DIP switches, display-only, in
// the shared LED visual language. Dark while the machine is powered off.
//
// It is titled after the board rather than "Front panel", which read as the
// machine's front panel — the card above it, which is exactly what these lamps
// and switches are not. A board is a QBone or a UniBone by the bus it carries.
//
// Neither row is labelled with what it carries, so both say it on hover: the
// lamps are the board's four drive-activity LEDs, and the switches are the
// board's own DIP block, which is read once at startup and not again.
const BOARD_LABEL: Record<string, string> = { QBUS: 'QBone', UNIBUS: 'UniBone' };
// The board's own name, for a backend that has said which bus it carries; one
// that has not yet (or runs on neither) leaves the panel named after the board
// in general terms rather than after the machine.
const boardPanelLabel = () => (BOARD_LABEL[store.platform] || 'Board') + ' panel';

const fpLedInfo = (i: number) =>
  'Drive activity LED ' + i + ' — pulses while a drive assigned to it reads or writes. ' +
  'Each drive carries an activity-LED number, which defaults to its unit number, so ' +
  'several drives can share one lamp. Dark while the emulation is off the bus.';
const fpDipInfo = (i: number) =>
  'DIP switch ' + i + ' — one of the board’s four physical switches, shown as it stands. ' +
  'They are read only when the backend starts, to choose which saved configuration to ' +
  'load; moving one now loads nothing. To use them, set them and restart the backend.';

function FrontPanel() {
  const s = useStore();
  const powered = s.hw.powered !== false;
  return html`<div class="card frontpanel"><div class="card-head"><h3>${boardPanelLabel()}</h3></div>
    <div class="card-body">
      <div class="fp-block">
        <div class="fp-cells fp-cells-led">${s.hw.leds.map(
          (v, i) => html`<div class="fp-cell" title=${fpLedInfo(i)}
            >${html`<${Led} on=${powered && v} title=${'Activity ' + i} info=${fpLedInfo(i)} />`}
            <span class="fp-n">${i}</span></div>`
        )}</div></div>
      <div class="fp-block">
        <div class="fp-cells fp-cells-sw">${s.hw.dip.map(
          (v, i) => html`<div class="fp-cell" title=${fpDipInfo(i + 1)}
            ><span class=${'dip' + (v ? ' on' : '')}></span>
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
// the dashboard. A console whose link is down is dimmed and says so: the
// terminal keeps whatever it last painted, and the card makes clear that what
// is on it is no longer live.
const CONSOLE_SOURCE_LABEL: Record<string, string> = {
  ttys2: 'ttyS2',
  webserial: 'Web Serial',
  dl11: 'DL11',
  vax: 'VAX console',
};
// The record control. A recording is made on the board, not here, because that
// is the only place both directions of the session pass: this browser sees the
// output every client gets, but not what another client typed. Off unless
// asked for — the input it captures includes whatever was typed at a password
// prompt, so a standing recording is not something to leave running.
function RecordButton() {
  const [on, setOn] = useState(false);
  const [busy, setBusy] = useState(false);
  const channel = consoleSource() === 'vax' ? 'vax' : consoleSource() === 'dl11' ? '0' : 'ext';
  useEffect(() => {
    let live = true;
    recordingState(channel)
      .then((s) => {
        if (live) setOn(s.recording);
      })
      .catch(() => {});
    // a recording started elsewhere (the API, another tab) shows up here
    const t = setInterval(() => {
      recordingState(channel)
        .then((s) => {
          if (live) setOn(s.recording);
        })
        .catch(() => {});
    }, 5000);
    return () => {
      live = false;
      clearInterval(t);
    };
  }, [channel]);
  const toggle = async () => {
    setBusy(true);
    try {
      const s = on ? await stopRecording(channel) : await startRecording(channel);
      setOn(s.recording);
    } catch {
      /* leave the button as it was; the next poll corrects it */
    }
    setBusy(false);
  };
  return html`<button
    class=${'btn small' + (on ? ' recording' : '')}
    disabled=${busy}
    title=${on ? 'Stop recording this session' : 'Record this session to a file on the board'}
    onClick=${toggle}
  >${on ? html`<span class="rec-dot"></span>Recording` : 'Record'}</button>`;
}

function ConsoleCard() {
  useStore();
  const live = store.consoleConnected;
  const src = CONSOLE_SOURCE_LABEL[consoleSource()] || '';
  return html`<div class=${'card console-card' + (live ? '' : ' console-down')}>
    <div class="card-head"><h3>Console</h3>
      <${RecordButton} />
      <span class=${'disk-status ' + (live ? 'ok' : 'idle')}
        >${live ? src : 'disconnected'}</span>
    </div>
    <div class="card-body dash-term">${html`<${TerminalHost} />`}</div>
  </div>`;
}

// ---- the arrangeable grid ----
const CELL = 47; // px per grid square
const GAP = 6;
// the always-present cards; their size, like every widget's, comes from measuring
// what they draw, so only their keys are fixed here
const FIXED = ['controlpanel', 'frontpanel', 'console'];
// the size a card is drawn at for the one frame before it is measured, in px: big
// enough that a card does not visibly grow into place, small enough not to shove
// the grid around. Every card starts here and settles to its measured size.
const PLACEHOLDER_PX = { w: 320, h: 220 };

// How many whole cells hold a run of `px`, a block of n cells being
// n·CELL + (n-1)·GAP wide. The half pixel keeps a sub-pixel layout rounding from
// buying a whole cell.
const cellsFor = (px: number): number =>
  Math.max(1, Math.ceil((px + GAP - 0.5) / (CELL + GAP)));

// The parts of a card that carry its content: everything but the head, whose
// title ellipsizes and must never widen a card.
const contentParts = (card: HTMLElement): HTMLElement[] =>
  (Array.from(card.children) as HTMLElement[]).filter((c) => !c.classList.contains('card-head'));

// The natural size of each card, read off the cards themselves and rounded up to
// whole cells. The card is stretched to fill its grid block, so it is briefly
// released — content parts to their max-content width, the card to auto height —
// to read the size the content lays out at without wrapping or stretching. The
// head is excluded from the width because its title ellipsizes and must never
// widen a card. This is the sole source of a card's size: no widget declares one.
//
// Read in one pass across the whole grid — widths (parts at max-content), then
// heights (card auto, parts still loose) — rather than a fresh layout per card.
function measureCards(cards: HTMLElement[]): (Cells | null)[] {
  const parts = cards.map(contentParts);
  const savedW = parts.map((ps) => ps.map((p) => p.style.width));
  parts.forEach((ps) => ps.forEach((p) => (p.style.width = 'max-content')));
  const widths = cards.map(
    (c, i) =>
      parts[i].reduce((m, p) => Math.max(m, p.getBoundingClientRect().width), 0) +
      (c.offsetWidth - c.clientWidth)
  );
  const savedH = cards.map((c) => c.style.height);
  cards.forEach((c) => (c.style.height = 'auto'));
  const heights = cards.map((c) => c.getBoundingClientRect().height);
  cards.forEach((c, i) => (c.style.height = savedH[i]));
  parts.forEach((ps, i) => ps.forEach((p, j) => (p.style.width = savedW[i][j])));
  return heights.map((h, i) => (h ? { w: cellsFor(widths[i]), h: cellsFor(h) } : null));
}
const FIXED_LABEL: Record<string, string> = {
  controlpanel: 'Control panel',
  console: 'Console',
};
// what a fixed card is called where it is named rather than drawn: the hidden
// tile in edit mode. The board's panel is named after the board it is on.
const fixedLabel = (key: string): string =>
  key === 'frontpanel' ? boardPanelLabel() : FIXED_LABEL[key] || key;
// a hidden card shows compact in edit mode: just enough cells for its name row
const HIDDEN_CELLS = { w: 6, h: 2 };

function renderCard(key: string, devByName: Record<string, LiveDev>, opts: WidgetOpts) {
  if (key === 'controlpanel') return html`<${ControlPanel} />`;
  if (key === 'frontpanel') return html`<${FrontPanel} />`;
  if (key === 'console') return html`<${ConsoleCard} />`;
  const d = devByName[key];
  return d ? html`<${WidgetCard} d=${d} opts=${opts} />` : null;
}

// A hidden widget in edit mode: represented by just its title, handle and a
// line of basic configuration, so the operator can find and re-show it without
// it taking a full widget's space.
function renderHidden(key: string, devByName: Record<string, LiveDev>) {
  const d = devByName[key];
  const title = d ? d.label || d.name : fixedLabel(key);
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
  // what each card measures, once it is on the page; the widgets' own sizes
  // stand in until then
  const [measured, setMeasured] = useState<Record<string, Cells>>({});
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
  // The widget options stored for a card. A card the layout does not name, or
  // one whose entry sets none, leaves every option at its widget's default.
  const optsOf = (key: string): WidgetOpts => layout[key]?.opts || {};

  // Every card takes the size it was measured at; before that it stands at the
  // shared placeholder. A hidden card shrinks to a compact tile in edit mode, and
  // every width is clamped so a card never exceeds the grid.
  // The console reads the machine's line and is as wide as its terminal, so with
  // no stored position it takes the row under the panels rather than the gap
  // beside them.
  const sized = (key: string): GridItem => {
    const newRow = key === 'console';
    if (edit && isHidden(key))
      return { key, w: Math.min(HIDDEN_CELLS.w, cols), h: HIDDEN_CELLS.h, newRow };
    const m = measured[key];
    return {
      key,
      w: Math.min(m ? m.w : cellsFor(PLACEHOLDER_PX.w), cols),
      h: m ? m.h : cellsFor(PLACEHOLDER_PX.h),
      newRow,
    };
  };
  const allItems: GridItem[] = [...FIXED, ...devs.map((d) => d.name)].map(sized);
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

  // Read every card and keep the grid's blocks in step with what the cards
  // draw. Only a change in whole cells is published, so a reflow of a few pixels
  // does not re-place the grid; and a measurement taken mid-drag is ignored, the
  // dragged card being where the operator holds it rather than where it lives.
  const measure = () => {
    const el = gridRef.current;
    if (!el || drag.current) return;
    const cards = Array.from(
      el.querySelectorAll<HTMLElement>('.grid-item[data-measure] > .card')
    );
    if (!cards.length) return;
    const found: Record<string, Cells> = {};
    measureCards(cards).forEach((m, i) => {
      const key = (cards[i].parentElement as HTMLElement).dataset.measure as string;
      if (m) found[key] = m;
    });
    setMeasured((prev) => {
      const next = { ...prev, ...found };
      // The control panel is the machine's bezel and the console the terminal in
      // front of it: they read as one stack, so the panel takes the console's
      // width rather than the narrower one its plate lays out at. Both are
      // measured in the same pass; a hidden console is not on the grid, and
      // leaves the panel at its own measured width.
      const cw = isHidden('console') ? 0 : next.console?.w;
      if (cw && next.controlpanel && next.controlpanel.w !== cw)
        next.controlpanel = { ...next.controlpanel, w: cw };
      let changed = false;
      for (const k in next) {
        const c = prev[k];
        if (!c || c.w !== next[k].w || c.h !== next[k].h) changed = true;
      }
      return changed ? next : prev;
    });
  };
  // What changes the shape of the cards: the set on the grid, the edit mode's
  // compact tiles, the column count a card is clamped to, and the layout, which
  // carries the widget options. Anything else a card does on its own — a font
  // arriving, the VCB01 canvas learning its size from the stream, a status
  // readout coming or going — is caught by the observer below.
  const shape = placed.map((p) => p.key).join(',') + '|' + cols + '|' + edit + '|' + JSON.stringify(layout);
  // measured before the browser paints, so a card settles into its block in the
  // frame it first appears in
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useLayoutEffect(measure, [shape]);
  useEffect(() => {
    const el = gridRef.current;
    if (!el) return;
    const ro = new ResizeObserver(measure);
    // Observe the content, not the card. The card is stretched to its block and
    // so keeps a fixed height; its content parts hold the true size and grow when
    // something arrives late — a font, a status readout, the VCB01 canvas learning
    // its size from the stream — which is exactly when a re-measure is due.
    el.querySelectorAll<HTMLElement>('.grid-item[data-measure] > .card').forEach((c) => {
      ro.observe(c);
      contentParts(c).forEach((p) => ro.observe(p));
    });
    return () => ro.disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shape]);

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

  // The switchable parts of a card's widget, offered in edit mode beside the
  // eye: one button per option the widget declares, lit while the option is on.
  // Turning one off resizes the card, so it belongs with the arrangement.
  const optionToggles = (key: string) => {
    const d = devByName[key];
    if (!d) return null;
    const list = widgetOptions(d);
    if (!list.length) return null;
    const cur = optsOf(key);
    return list.map((o) => {
      const on = typeof cur[o.key] === 'boolean' ? cur[o.key] : o.def;
      return html`<button class=${'gi-opt' + (on ? ' on' : '')}
        title=${o.label + ' — ' + o.info}
        onMouseDown=${(e: Event) => e.stopPropagation()}
        onClick=${() => patch(key, { opts: { ...cur, [o.key]: !on } })}>${o.label}</button>`;
    });
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

  // the layout controls sit with the cog, on the right of the heading, since
  // both are about the screen rather than about the machine on it
  const tools = edit
    ? html`<span class="muted dash-edit-hint">drag a card to move it; the eye hides it</span>
        <button class="btn small primary" disabled=${!dirty} onClick=${save}>Save layout</button>
        <button class="btn small" onClick=${revert}>Revert</button>`
    : html`<button class="btn small" onClick=${() => setEdit(true)}>Edit layout</button>`;

  return html`<${DashHeader} tools=${tools} />
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
          data-measure=${edit && hidden ? null : p.key}
          style=${'grid-column:' + (x + 1) + '/span ' + p.w + ';grid-row:' + (y + 1) + '/span ' + p.h + ';'}>
          ${
            edit && hidden
              ? renderHidden(p.key, devByName)
              : renderCard(p.key, devByName, optsOf(p.key))
          }
          ${
            edit
              ? html`<div class="gi-edit" onMouseDown=${onDown(p.key)}>
                  <span class="gi-move mono">⠿ ${p.key}</span>
                  <div class="gi-tools">
                    ${hidden ? null : optionToggles(p.key)}
                    <button class="gi-eye" title=${hidden ? 'show' : 'hide'}
                      onMouseDown=${(e: Event) => e.stopPropagation()}
                      onClick=${() => patch(p.key, { hidden: !hidden })}>${hidden ? '🙈' : '👁'}</button>
                  </div>
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
  // Typing on this screen is typing at the machine. Operating a panel switch
  // leaves the focus on the button that was pressed, and the next keystroke
  // would go there instead of to the console, so the caret is put back after
  // anything that moves it — unless a field on the page wants it.
  useEffect(() => {
    const back = () => setTimeout(focusConsole, 0);
    document.addEventListener('mouseup', back);
    document.addEventListener('focusin', back);
    return () => {
      document.removeEventListener('mouseup', back);
      document.removeEventListener('focusin', back);
    };
  }, []);
  return html`<section class="page active" data-page="dashboard">
    ${html`<${DashGrid} />`}
  </section>`;
}
