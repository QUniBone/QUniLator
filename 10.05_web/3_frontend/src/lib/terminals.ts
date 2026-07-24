// xterm.js terminals + Web Serial. Three terminal instances live in module
// state inside a persistent root element; the dashboard and the standalone
// /console page both mount a host that this root is re-parented into, so
// navigating away and back keeps the live instances and their WebSockets
// rather than tearing the console down.
import { Terminal } from '@xterm/xterm';
import '@xterm/xterm/css/xterm.css';
import { store, setStore, emit } from '../store';
import { wsURL } from './util';
import { toast } from './toast';
import { devEnabled } from './devmodel';
import type { TermKey } from '../types';

const COLS = 80,
  ROWS = 24;

interface TermInst {
  el: HTMLDivElement;
  term: Terminal;
  ws?: WebSocket;
}
type Terms = Record<TermKey, TermInst>;

let TERMS: Terms | null = null;
let termRoot: HTMLDivElement | null = null;
let extWs: WebSocket | null = null;
let serialPort: WebSerialPort | null = null;
let serialWriter: WritableStreamDefaultWriter<Uint8Array> | null = null;
let serialReader: ReadableStreamDefaultReader<Uint8Array> | null = null;
const serialEncoder = new TextEncoder();

export function serialConnected(): boolean {
  return serialPort != null;
}

function makeTermInstance(): TermInst {
  const cs = getComputedStyle(document.documentElement);
  const phosphor = cs.getPropertyValue('--phosphor').trim() || '#7CE38B';
  const el = document.createElement('div');
  el.style.display = 'none';
  termRoot!.appendChild(el);
  const term = new Terminal({
    cols: COLS,
    rows: ROWS,
    cursorBlink: true,
    scrollback: 1000,
    fontFamily: cs.getPropertyValue('--font-mono').trim() || 'monospace',
    fontSize: 14,
    theme: {
      background: cs.getPropertyValue('--term-bg').trim() || '#0B100C',
      foreground: phosphor,
      cursor: phosphor,
    },
  });
  term.open(el);
  return { el, term };
}

export function initLiveTerminal(host: HTMLElement): void {
  const first = !TERMS;
  if (first) {
    termRoot = document.createElement('div');
    termRoot.className = 'term-root';
    TERMS = { slu0: makeTermInstance(), slu1: makeTermInstance(), serial: makeTermInstance() };
    wireConsole('slu0', 0);
    wireConsole('slu1', 1);
    wireSerial();
    setStore({ termReady: true });
  }
  // move the persistent root into the freshly mounted host
  host.appendChild(termRoot!);
  if (first) {
    // the default tab is an SLU that may be disabled; fall back to the console
    const en0 = devEnabled('DL11'),
      en1 = devEnabled('DL11b');
    const start: TermKey =
      (store.activeTerm === 'slu0' && !en0) || (store.activeTerm === 'slu1' && !en1)
        ? en0
          ? 'slu0'
          : en1
            ? 'slu1'
            : 'serial'
        : store.activeTerm;
    liveTab(start);
    updateConsoleSource();
  } else {
    TERMS![store.activeTerm].term.focus();
  }
}

function wireConsole(key: TermKey, n: number): void {
  const t = TERMS![key];
  t.term.onData((d) => {
    if (t.ws && t.ws.readyState === WebSocket.OPEN) t.ws.send(d);
  });
  (function connect() {
    t.ws = new WebSocket(wsURL('/ws/console/' + n));
    t.ws.binaryType = 'arraybuffer';
    t.ws.onmessage = (e) => t.term.write(new Uint8Array(e.data as ArrayBuffer));
    t.ws.onclose = () => setTimeout(connect, 2000);
  })();
}

export function liveTab(key: TermKey): void {
  if (!TERMS) return;
  store.activeTerm = key;
  for (const k of Object.keys(TERMS) as TermKey[]) TERMS[k].el.style.display = k === key ? '' : 'none';
  TERMS[key].term.focus();
  emit();
}

function extConsoleDisconnectWs(): void {
  if (extWs) {
    try {
      extWs.close();
    } catch {
      /* ignore */
    }
    extWs = null;
  }
}

function wireExtConsoleWs(): void {
  const t = TERMS!.serial;
  extConsoleDisconnectWs();
  (function connect() {
    if ((store.settings.external_console || {}).source !== 'ttys2') return;
    extWs = new WebSocket(wsURL('/ws/console/ext'));
    extWs.binaryType = 'arraybuffer';
    extWs.onmessage = (e) => t.term.write(new Uint8Array(e.data as ArrayBuffer));
    extWs.onclose = () => {
      extWs = null;
      if ((store.settings.external_console || {}).source === 'ttys2') setTimeout(connect, 2000);
    };
  })();
}

export function updateConsoleSource(): void {
  if (!TERMS) return;
  const src = (store.settings.external_console || {}).source || 'off';
  if (serialPort) serialDisconnect();
  extConsoleDisconnectWs();
  if (src === 'ttys2') wireExtConsoleWs();
  emit();
}

export async function serialConnect(baudRate: number): Promise<void> {
  if (!TERMS || !navigator.serial) return;
  const t = TERMS.serial;
  let port: WebSerialPort;
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate });
  } catch (e) {
    const err = e as Error;
    if (err.name !== 'NotFoundError') toast('serial', 'cannot open port: ' + err.message);
    return;
  }
  serialPort = port;
  serialWriter = port.writable!.getWriter();
  localStorage.setItem('webserial.baud', String(baudRate));
  emit();
  toast('serial', 'connected at ' + baudRate + ' baud');
  try {
    while (port.readable) {
      serialReader = port.readable.getReader();
      try {
        while (true) {
          const { value, done } = await serialReader.read();
          if (done) break;
          if (value) t.term.write(value);
        }
      } finally {
        serialReader.releaseLock();
        serialReader = null;
      }
    }
  } catch (e) {
    toast('serial', 'read error: ' + (e as Error).message);
  }
  await serialDisconnect();
}

export async function serialDisconnect(): Promise<void> {
  if (!serialPort) return;
  const port = serialPort;
  serialPort = null;
  try {
    if (serialReader) await serialReader.cancel();
  } catch {
    /* ignore */
  }
  try {
    if (serialWriter) {
      serialWriter.releaseLock();
      serialWriter = null;
    }
  } catch {
    /* ignore */
  }
  try {
    await port.close();
  } catch {
    /* ignore */
  }
  emit();
  toast('serial', 'disconnected');
}

function wireSerial(): void {
  const t = TERMS!.serial;
  t.term.onData((d) => {
    if ((store.settings.external_console || {}).source === 'ttys2') {
      if (extWs && extWs.readyState === WebSocket.OPEN) extWs.send(d);
    } else if (serialWriter) serialWriter.write(serialEncoder.encode(d));
  });
  if (navigator.serial)
    navigator.serial.addEventListener('disconnect', (e) => {
      if (serialPort && e.target === serialPort) serialDisconnect();
    });
}

// closes every terminal socket; used by the pagehide teardown
export function shutdownTerminals(): void {
  const shut = (ws: WebSocket | null | undefined) => {
    try {
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
    } catch {
      /* ignore */
    }
  };
  shut(extWs);
  if (TERMS) for (const k of Object.keys(TERMS) as TermKey[]) shut(TERMS[k].ws);
}
