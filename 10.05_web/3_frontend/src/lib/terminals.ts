// The console terminal: one xterm.js instance for the machine's one console.
//
// A PDP-11 has a single console, and which port carries it is a machine
// setting, not a choice made at the terminal: the BeagleBone's ttyS2 bridged to
// a physical console SLU, a Web Serial port opened in the browser, or - when
// neither - the emulated DL11 at 777560. The terminal follows that setting.
// Every other serial line is reached over its own TCP port, not through this
// card, so there is one terminal here and nothing to switch between.
//
// The terminal follows the router's lifecycle: a mount builds a fresh Terminal
// against the live host DOM and connects the console's channel; the server
// replays the channel's retained history on connect, so the screen repaints. An
// unmount closes the socket and disposes the terminal. Nothing survives in
// module state across a route change, so there is no detached host or stale
// instance to leave a black screen behind.
//
// The Web Serial session is the exception: it is a user-granted hardware port,
// not a channel socket, so it stays open across navigation and its read loop
// writes to whichever terminal is currently mounted (dropping bytes while the
// console is unmounted, since Web Serial has no server-side replay).
import { Terminal } from '@xterm/xterm';
import '@xterm/xterm/css/xterm.css';
import { store, setStore, emit } from '../store';
import { wsURL } from './util';
import { toast } from './toast';
import type { ConsoleSource } from '../types';

const COLS = 80,
  ROWS = 24;

let term: Terminal | null = null;
let termEl: HTMLDivElement | null = null;
let hostEl: HTMLElement | null = null;
// the channel socket backing the console, when a channel carries it
let ws: WebSocket | null = null;
// the channel path connectConsole last wired up (null: the browser's serial
// port carries the console), so a refresh can tell a moved console from an
// unchanged one
let wiredPath: string | null = null;
let serialPort: WebSerialPort | null = null;
let serialWriter: WritableStreamDefaultWriter<Uint8Array> | null = null;
let serialReader: ReadableStreamDefaultReader<Uint8Array> | null = null;
const serialEncoder = new TextEncoder();
let serialDisconnectHooked = false;
// true while the replayed history is being written to xterm, so answerbacks to
// replayed query escapes are suppressed
let replaying = false;
// the first binary frame after connect is the retained-history replay
let replayPending = false;
// this console is the server-designated terminal answerer (only it answers the
// guest's identification queries, so several open consoles do not each reply)
let answerer = false;

export function serialConnected(): boolean {
  return serialPort != null;
}

// Where the console lives. A VAX carries its console terminal inside the
// processor rather than on the bus, so a machine with one running has its
// console there and nowhere else. Otherwise the external-console setting names
// the physical path, and with none the console is the emulated DL11 the guest
// talks to.
export function consoleSource(): ConsoleSource {
  if (store.devmodel.some((d) => d.type === 'VAX-11/780' && d.enabled)) return 'vax';
  const src = (store.settings.external_console || {}).source || 'off';
  if (src === 'ttys2') return 'ttys2';
  if (src === 'webserial') return 'webserial';
  return 'dl11';
}

// The console's channel path, or null when the browser's own serial port
// carries it and there is no server channel to connect.
function consolePath(): string | null {
  const src = consoleSource();
  if (src === 'ttys2') return '/ws/console/ext';
  if (src === 'dl11') return '/ws/console/0';
  if (src === 'vax') return '/ws/console/vax';
  return null;
}

function setConnected(connected: boolean): void {
  if (store.consoleConnected !== connected) setStore({ consoleConnected: connected });
}

// close a socket without triggering its reconnect timer
function closeWs(sock: WebSocket | null | undefined): void {
  if (!sock) return;
  try {
    sock.onclose = null;
    sock.close();
  } catch {
    /* ignore */
  }
}

// Mount: build a fresh terminal into the host and connect it to whatever
// carries the console. Called from the TerminalHost component's mount effect,
// after the host div is committed to the DOM.
export function initLiveTerminal(host: HTMLElement): void {
  // never stack instances: a remount without a matching unmount rebuilds clean
  if (term) teardownTerminals();
  hostEl = host;
  const cs = getComputedStyle(document.documentElement);
  const phosphor = cs.getPropertyValue('--phosphor').trim() || '#7CE38B';
  termEl = document.createElement('div');
  hostEl.appendChild(termEl);
  term = new Terminal({
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
  term.open(termEl);
  wireTerminal();
  connectConsole();
  setStore({ termReady: true });
  term.focus();
}

// Unmount: close the channel socket and dispose the terminal. The Web Serial
// port stays connected; its read loop simply finds no terminal and drops bytes
// until the console remounts.
export function teardownTerminals(): void {
  if (!term) return;
  closeWs(ws);
  ws = null;
  wiredPath = null;
  try {
    term.dispose();
  } catch {
    /* ignore */
  }
  termEl?.remove();
  term = null;
  termEl = null;
  hostEl = null;
  setConnected(false);
}

// Keystrokes go to whatever carries the console.
function wireTerminal(): void {
  const t = term!;
  // Answer the guest's terminal-identification queries here rather than through
  // xterm's built-in auto-reply. Returning true from these handlers suppresses
  // that built-in reply on every console, and only the server-designated
  // answerer actually emits a response — so N mirrored consoles yield one
  // answer, not N — and never for a query replayed from the retained history.
  // primary Device Attributes: VT100 with AVO. A VT100 answers DECID (ESC Z)
  // with the same report, and guests reach for either — VMS's SET
  // TERMINAL/INQUIRE sends both in turn — so both handlers reply with it.
  const identify = () => {
    if (answerer && !replaying && ws && ws.readyState === WebSocket.OPEN) ws.send('\x1b[?1;2c');
    return true;
  };
  t.parser.registerCsiHandler({ final: 'c' }, identify);
  t.parser.registerEscHandler({ final: 'Z' }, identify);
  t.onData((d) => {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(d);
    else if (serialWriter) serialWriter.write(serialEncoder.encode(d));
  });
  // Selecting console text copies it. A terminal has no other use for a
  // selection — there is nothing to drag it onto — so the gesture that makes
  // one is taken to mean "I want this", and the operator is spared a second
  // one. Writing to the clipboard needs the document focused, which it is,
  // having just been clicked in; a browser that refuses is ignored, since the
  // selection is still there to copy by hand.
  t.onSelectionChange(() => {
    const text = t.getSelection();
    if (!text || !navigator.clipboard) return;
    navigator.clipboard.writeText(text).catch(() => {});
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

// Connect the console channel, and keep reconnecting while this mount lives: a
// service restart or a machine that is not yet up is a channel that comes back.
function connectConsole(): void {
  closeWs(ws);
  ws = null;
  const path = consolePath();
  wiredPath = path;
  if (path == null) {
    // the browser's serial port carries it; the read loop reports the state
    setConnected(serialPort != null);
    return;
  }
  const mount = term;
  (function connect() {
    // stop reconnecting once this mount has been torn down or the source moved
    if (!term || term !== mount || consolePath() !== path) return;
    const sock = new WebSocket(wsURL(path));
    ws = sock;
    sock.binaryType = 'arraybuffer';
    sock.onopen = () => {
      // The server re-evaluates the answerer on every connection and replays the
      // retained history first. Default to not answering, and suppress
      // answerbacks until that replay has been written, so a reconnect neither
      // answers as a duplicate terminal nor re-answers a query already in the
      // history (a replayed answer would land as stray input at the guest's
      // current prompt). Cleared once xterm has written the replayed history.
      answerer = false;
      replaying = true;
      replayPending = true;
      // safety net in case the replay write callback is never delivered
      setTimeout(() => {
        replaying = false;
      }, 4000);
      // clear before the ring replay so a reconnect repaints from the replay
      // rather than appending a second copy of the history
      term?.reset();
      setConnected(true);
    };
    sock.onmessage = (e) => {
      // control frames (answerer designation) arrive as text; terminal data is
      // binary and the first binary frame after connect is the history replay
      if (typeof e.data === 'string') {
        try {
          const msg = JSON.parse(e.data);
          if (typeof msg.answerer === 'boolean') answerer = msg.answerer;
        } catch {
          /* ignore malformed control frame */
        }
        return;
      }
      if (!term) return;
      const data = new Uint8Array(e.data as ArrayBuffer);
      if (replayPending) {
        replayPending = false;
        term.write(data, () => {
          replaying = false;
        });
      } else {
        term.write(data);
      }
    };
    sock.onclose = () => {
      if (ws === sock) ws = null;
      setConnected(false);
      setTimeout(connect, 2000);
    };
  })();
}

// The console moved to another port: rebuild the connection for the new one.
// Both halves of what names the console can move — the external-console setting
// and the device set that says whether a VAX is running — so this is called
// after either is reloaded. A call that finds the console still on the same
// channel leaves the live socket alone, so a routine device refresh does not
// tear down a working terminal and replay its history.
export function updateConsoleSource(): void {
  if (!term) return;
  if (consolePath() === wiredPath) return;
  if (consolePath() != null && serialPort) serialDisconnect();
  connectConsole();
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
  if (consolePath() == null) setConnected(true);
  emit();
  toast('serial', 'connected at ' + baudRate + ' baud');
  try {
    while (port.readable) {
      serialReader = port.readable.getReader();
      try {
        while (true) {
          const { value, done } = await serialReader.read();
          if (done) break;
          if (value && term) term.write(value);
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
  if (consolePath() == null) setConnected(false);
  emit();
  toast('serial', 'disconnected');
}

// closes the console socket; used by the pagehide teardown
export function shutdownTerminals(): void {
  closeWs(ws);
}

// The machine was switched on: it has printed nothing yet, so the screen is
// cleared rather than left showing what the last one printed. The server has
// already forgotten its retained history, so a reload agrees with what is here.
export function clearConsole(): void {
  term?.reset();
}

// Put the keyboard back in the console. The dashboard is a machine's front
// panel: its switches are operated with the mouse, and anything typed is meant
// for the machine, so the caret belongs in the terminal whenever nothing on the
// page is asking for text itself.

export function focusConsole(): void {
  if (!term) return;
  const active = document.activeElement as HTMLElement | null;
  if (active && (active.tagName === 'INPUT' || active.tagName === 'TEXTAREA'
      || active.tagName === 'SELECT' || active.isContentEditable))
    return;
  term.focus();
}
