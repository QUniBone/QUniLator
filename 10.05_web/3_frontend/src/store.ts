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
  TermKey,
} from './types';

export interface Store {
  devmodel: LiveDev[];
  settings: Settings;
  images: ImageInfo[];
  imagesDir: string;
  configs: ConfigSummary[] | null;
  platform: string;
  connected: boolean;
  bus: BusState;
  hw: HwState;
  log: LogLine[];
  activeLevels: Set<LogLevelName>;
  termReady: boolean;
  activeTerm: TermKey;
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
  imagesDir: '',
  configs: null,
  platform: '',
  connected: false,
  bus: { halted: false, init: false },
  hw: { dcok: true, pok: true, leds: [false, false, false, false], dip: [false, false, false, false] },
  log: [],
  activeLevels: new Set<LogLevelName>(['ERROR', 'WARNING', 'INFO']),
  termReady: false,
  activeTerm: 'slu0',
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
