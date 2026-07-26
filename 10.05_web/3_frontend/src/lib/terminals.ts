// xterm.js terminals + Web Serial. The terminals follow the router's lifecycle:
// a mount builds three fresh Terminal instances against the live host DOM and
// connects their channel WebSockets; the server replays each channel's retained
// history on connect, so the screen repaints. An unmount closes those sockets
// and disposes the terminals. Nothing survives in module state across a route
// change, so there is no detached-host or stale-instance to leave a black
// screen behind.
//
// The Web Serial session is the exception: it is a user-granted hardware port,
// not a channel socket, so it stays open across navigation and its read loop
// writes to whichever serial terminal is currently mounted (dropping bytes while
// the console is unmounted, since Web Serial has no server-side replay).
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
let hostEl: HTMLElement | null = null;
let extWs: WebSocket | null = null;
let serialPort: WebSerialPort | null = null;
let serialWriter: WritableStreamDefaultWriter<Uint8Array> | null = null;
let serialReader: ReadableStreamDefaultReader<Uint8Array> | null = null;
const serialEncoder = new TextEncoder();
let serialDisconnectHooked = false;
// true while the external console's replayed history is being written to xterm,
// so answerbacks to replayed query escapes are suppressed
let extReplaying = false;
// the first binary frame after connect is the retained-history replay
let extReplayPending = false;
// this console is the server-designated terminal answerer (only it answers the
// guest's identification queries, so several open consoles do not each reply)
let extAnswerer = false;

export function serialConnected(): boolean {
  return serialPort != null;
}

// the current serial terminal, or null while the console is unmounted
function serialTerm(): Terminal | null {
  return TERMS ? TERMS.serial.term : null;
}

// close a socket without triggering its reconnect timer
function closeWs(ws: WebSocket | null | undefined): void {
  if (!ws) return;
  try {
    ws.onclose = null;
    ws.close();
  } catch {
    /* ignore */
  }
}

function makeTermInstance(visible: boolean): TermInst {
  const cs = getComputedStyle(document.documentElement);
  const phosphor = cs.getPropertyValue('--phosphor').trim() || '#7CE38B';
  const el = document.createElement('div');
  el.style.display = visible ? '' : 'none';
  // append into the live host first so term.open() runs against an element that
  // is in the document with layout — the active tab is visible, so its cursor
  // renders immediately even before the first byte arrives
  hostEl!.appendChild(el);
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

// Mount: build fresh terminals into the host and connect. Called from the
// TerminalHost component's mount effect, after the host div is committed to the
// DOM.
export function initLiveTerminal(host: HTMLElement): void {
  // never stack instances: a remount without a matching unmount rebuilds clean
  if (TERMS) teardownTerminals();
  hostEl = host;
  // the stored tab may be an SLU that is disabled; fall back to the console
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
  TERMS = {
    slu0: makeTermInstance(start === 'slu0'),
    slu1: makeTermInstance(start === 'slu1'),
    serial: makeTermInstance(start === 'serial'),
  };
  store.activeTerm = start;
  wireConsole('slu0', 0);
  wireConsole('slu1', 1);
  wireSerial();
  updateConsoleSource();
  setStore({ termReady: true });
  TERMS[start].term.focus();
}

// Unmount: close the channel sockets and dispose the terminals. The Web Serial
// port stays connected; its read loop simply finds no terminal and drops bytes
// until the console remounts.
export function teardownTerminals(): void {
  if (!TERMS) return;
  for (const k of Object.keys(TERMS) as TermKey[]) {
    const inst = TERMS[k];
    closeWs(inst.ws);
    try {
      inst.term.dispose();
    } catch {
      /* ignore */
    }
    inst.el.remove();
  }
  extConsoleDisconnectWs();
  TERMS = null;
  hostEl = null;
}

function wireConsole(key: TermKey, n: number): void {
  const t = TERMS![key];
  t.term.onData((d) => {
    if (t.ws && t.ws.readyState === WebSocket.OPEN) t.ws.send(d);
  });
  (function connect() {
    // stop reconnecting once this mount has been torn down
    if (!TERMS || TERMS[key] !== t) return;
    t.ws = new WebSocket(wsURL('/ws/console/' + n));
    t.ws.binaryType = 'arraybuffer';
    // the server replays the full ring on connect; clear the screen first so a
    // reconnect repaints from the replay rather than appending a second copy of
    // the history onto the surviving terminal
    t.ws.onopen = () => t.term.reset();
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
  closeWs(extWs);
  extWs = null;
}

function wireExtConsoleWs(): void {
  extConsoleDisconnectWs();
  (function connect() {
    if (!TERMS || (store.settings.external_console || {}).source !== 'ttys2') return;
    extWs = new WebSocket(wsURL('/ws/console/ext'));
    extWs.binaryType = 'arraybuffer';
    // clear before the ring replay so a reconnect repaints rather than doubling
    extWs.onopen = () => {
      const t = serialTerm();
      // The server re-evaluates the answerer on every connection and replays the
      // retained history first. Default to not answering, and suppress
      // answerbacks until that replay has been written, so a reconnect neither
      // answers as a duplicate terminal nor re-answers a query already in the
      // history (a replayed answer would land as stray input at the guest's
      // current prompt). Cleared once xterm has written the replayed history.
      extAnswerer = false;
      extReplaying = true;
      extReplayPending = true;
      // safety net in case the replay write callback is never delivered
      setTimeout(() => { extReplaying = false; }, 4000);
      if (t) t.reset();
    };
    extWs.onmessage = (e) => {
      // control frames (answerer designation) arrive as text; terminal data is
      // binary and the first binary frame after connect is the history replay
      if (typeof e.data === 'string') {
        try {
          const msg = JSON.parse(e.data);
          if (typeof msg.answerer === 'boolean') extAnswerer = msg.answerer;
        } catch {
          /* ignore malformed control frame */
        }
        return;
      }
      const t = serialTerm();
      if (!t) return;
      const data = new Uint8Array(e.data as ArrayBuffer);
      if (extReplayPending) {
        extReplayPending = false;
        t.write(data, () => {
          extReplaying = false;
        });
      } else {
        t.write(data);
      }
    };
    extWs.onclose = () => {
      extWs = null;
      if (TERMS && (store.settings.external_console || {}).source === 'ttys2') setTimeout(connect, 2000);
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
  if (!navigator.serial) return;
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
          const t = serialTerm();
          if (value && t) t.write(value);
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
  // Answer the guest's terminal-identification queries here rather than through
  // xterm's built-in auto-reply. Returning true from these handlers suppresses
  // that built-in reply on every console, and only the server-designated
  // answerer actually emits a response — so N mirrored consoles yield one answer,
  // not N — and never for a query replayed from the retained history.
  t.term.parser.registerCsiHandler({ final: 'c' }, () => {
    if (extAnswerer && !extReplaying && extWs && extWs.readyState === WebSocket.OPEN)
      extWs.send('\x1b[?1;2c'); // primary Device Attributes: VT100 with AVO
    return true;
  });
  t.term.parser.registerEscHandler({ final: 'Z' }, () => true); // suppress DECID auto-reply
  t.term.onData((d) => {
    if ((store.settings.external_console || {}).source === 'ttys2') {
      if (extWs && extWs.readyState === WebSocket.OPEN) extWs.send(d);
    } else if (serialWriter) serialWriter.write(serialEncoder.encode(d));
  });
  // the physical-disconnect hook lives on navigator.serial, which outlives every
  // mount, so register it once rather than on each rebuild
  if (navigator.serial && !serialDisconnectHooked) {
    serialDisconnectHooked = true;
    navigator.serial.addEventListener('disconnect', (e) => {
      if (serialPort && e.target === serialPort) serialDisconnect();
    });
  }
}

// closes every terminal socket; used by the pagehide teardown
export function shutdownTerminals(): void {
  closeWs(extWs);
  if (TERMS) for (const k of Object.keys(TERMS) as TermKey[]) closeWs(TERMS[k].ws);
}
