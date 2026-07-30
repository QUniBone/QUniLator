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
  connected: boolean;
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
    external_console: { source: 'webserial', port: 'ttyS2', baud: 38400 },
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
  connected: false,
  bus: { halted: false, init: false },
  hw: {
    dcok: true,
    pok: true,
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
