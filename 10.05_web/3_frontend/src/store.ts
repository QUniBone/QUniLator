// Shared store: one mutable object plus a listener set. Components read it
// through useStore() and re-render when any field changes. High-frequency lamp
// events are coalesced with a short timer so a busy device does not thrash the
// whole tree.
import { useEffect, useState } from 'preact/hooks';
import type {
  Settings,
  LiveDev,
  ImageInfo,
  ConfigSummary,
  LogLine,
  HwState,
  BusState,
  LogLevelName,
  UpdateStatus,
} from './types';

export interface Store {
  devmodel: LiveDev[];
  settings: Settings;
  images: ImageInfo[];
  dirs: string[]; // image-folder subpaths (the tree the storage page walks)
  imagesDir: string;
  configs: ConfigSummary[] | null;
  configCurrent: string; // the running configuration's name
  configModified: boolean | null; // live setup differs from saved current; null when the busy machine blocked the compare
  platform: string;
  // what the board runs, from GET /api/version: the package that owns the
  // binary, its version and its build timestamp
  serverPackage: string;
  serverVersion: string;
  serverBuilt: string;
  // the self-update status, from GET /api/update and the "update" event frame;
  // null until the first one arrives
  update: UpdateStatus | null;
  connected: boolean;
  // What holds the board, from the state frame's `held_by`: the checks a
  // power-up runs, or the interactive menu having the hardware. Empty when
  // nothing does. The board answers 409 to anything that would change the
  // machine while it is set, so the page locks rather than letting an operator
  // press buttons that will be refused.
  heldBy: string;
  // The board's standing notice, from the state frame's `notice`: something it
  // did on its own that no request of the operator's would show them - a
  // configuration that came up running unattended because it was marked to.
  // Empty when there is none. It stands until dismissed, and the dismissal is
  // the only record that a person saw it.
  notice: string;
  bus: BusState;
  hw: HwState;
  log: LogLine[]; // ascending by id; newest appended, older pages prepended
  logMore: boolean; // older entries remain in the journal to page in
  activeLevels: Set<LogLevelName>;
  termReady: boolean;
  // the console terminal has a live link to whatever carries the console
  consoleConnected: boolean;
  listeners: Set<() => void>;
}

export const store: Store = {
  devmodel: [],
  settings: {
    platform: '',
    address_width: 22,
    external_console: { source: 'ttys2', port: 'ttyS2', baud: 38400 },
  },
  images: [],
  dirs: [],
  imagesDir: '',
  configs: null,
  configCurrent: '',
  configModified: null,
  platform: '',
  serverPackage: '',
  serverVersion: '',
  serverBuilt: '',
  update: null,
  connected: false,
  heldBy: '',
  notice: '',
  bus: { halted: false, init: false },
  hw: {
    // unknown until a state frame says otherwise: a page that has not heard
    // from the board has not measured anything
    dcok: null,
    pok: null,
    // the live board (pre-deploy) never sends `powered`; default on so the
    // machine reads powered-up until a dc_off arrives
    powered: true,
    leds: [false, false, false, false],
    dip: [false, false, false, false],
  },
  log: [],
  logMore: false,
  activeLevels: new Set<LogLevelName>(['ERROR', 'WARNING', 'INFO']),
  termReady: false,
  consoleConnected: false,
  listeners: new Set<() => void>(),
};

let emitPending = false;
export function emit(): void {
  if (emitPending) return;
  emitPending = true;
  queueMicrotask(() => {
    emitPending = false;
    store.listeners.forEach((l) => l());
  });
}

export function setStore(patch: Partial<Store>): void {
  Object.assign(store, patch);
  emit();
}

let widgetEmitPending = false;
export function emitSoon(): void {
  // coalesce lamp storms
  if (widgetEmitPending) return;
  widgetEmitPending = true;
  setTimeout(() => {
    widgetEmitPending = false;
    store.listeners.forEach((l) => l());
  }, 150);
}

export function useStore(): Store {
  const [, force] = useState(0);
  useEffect(() => {
    const l = () => force((x) => x + 1);
    store.listeners.add(l);
    return () => {
      store.listeners.delete(l);
    };
  }, []);
  return store;
}
