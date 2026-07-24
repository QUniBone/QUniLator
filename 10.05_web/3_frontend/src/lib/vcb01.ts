// VCB01 framebuffer over /ws/vcb01: incremental row updates blitted to the
// widget canvas, plus keyboard and mouse input back to the board.
import { wsURL } from './util';

interface Vcb01State {
  ws: WebSocket | null;
  off: HTMLCanvasElement | null;
  w: number;
  h: number;
}
const VCB01D: Vcb01State = { ws: null, off: null, w: 0, h: 0 };

function vcb01Off(): HTMLCanvasElement {
  if (!VCB01D.off) VCB01D.off = document.createElement('canvas');
  return VCB01D.off;
}

// the picture wears the terminal's phosphor: lit pixels in --phosphor on the
// --term-bg ground, so the screen and the console read as one CRT
const VCB01_INK = (() => {
  const cs = getComputedStyle(document.documentElement);
  const rgb = (name: string, dflt: string): [number, number, number] => {
    const h = (cs.getPropertyValue(name).trim() || dflt).replace('#', '');
    return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
  };
  return { on: rgb('--phosphor', '3DF57F'), off: rgb('--term-bg', '0B100C') };
})();

function vcb01DrawRows(first: number, count: number, bytes: Uint8Array, boff: number): void {
  const w = VCB01D.w,
    stride = w >> 3;
  const [onR, onG, onB] = VCB01_INK.on,
    [offR, offG, offB] = VCB01_INK.off;
  const ctx = vcb01Off().getContext('2d')!;
  const img = ctx.createImageData(w, count),
    d = img.data;
  for (let r = 0; r < count; r++)
    for (let x = 0; x < w; x++) {
      const on = (bytes[boff + r * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
      const p = (r * w + x) * 4;
      d[p] = on ? onR : offR;
      d[p + 1] = on ? onG : offG;
      d[p + 2] = on ? onB : offB;
      d[p + 3] = 255;
    }
  ctx.putImageData(img, 0, first);
}

export function vcb01Blit(): void {
  const cv = document.getElementById('vcb01-canvas') as HTMLCanvasElement | null;
  if (!cv || !VCB01D.w) return;
  if (cv.width !== VCB01D.w || cv.height !== VCB01D.h) {
    cv.width = VCB01D.w;
    cv.height = VCB01D.h;
  }
  cv.getContext('2d')!.drawImage(VCB01D.off!, 0, 0);
}

export function vcb01Connect(): void {
  if (VCB01D.ws && VCB01D.ws.readyState <= WebSocket.OPEN) return;
  const ws = new WebSocket(wsURL('/ws/vcb01'));
  ws.binaryType = 'arraybuffer';
  ws.onmessage = (ev) => {
    const b = new Uint8Array(ev.data as ArrayBuffer);
    if (b[0] === 1) {
      VCB01D.w = (b[1] << 8) | b[2];
      VCB01D.h = (b[3] << 8) | b[4];
      vcb01Off().width = VCB01D.w;
      vcb01Off().height = VCB01D.h;
      vcb01DrawRows(0, VCB01D.h, b, 5);
    } else if (b[0] === 2 && VCB01D.w) vcb01DrawRows((b[1] << 8) | b[2], (b[3] << 8) | b[4], b, 5);
    else return;
    vcb01Blit();
  };
  // a reaped or dropped socket would otherwise freeze the screen; reconnect
  // while the widget is still on the page (the server closes idle duplicates)
  ws.onclose = () => {
    VCB01D.ws = null;
    if (document.getElementById('vcb01-canvas')) setTimeout(vcb01Connect, 1500);
  };
  VCB01D.ws = ws;
}

export function vcb01Disconnect(): void {
  if (VCB01D.ws) {
    const w = VCB01D.ws;
    VCB01D.ws = null;
    w.onclose = null;
    w.close();
  }
}

export function vcb01Socket(): WebSocket | null {
  return VCB01D.ws;
}

const VCB01_KEYSYM: Record<string, number> = (() => {
  const m: Record<string, number> = {};
  for (let c = 65; c <= 90; c++) m['Key' + String.fromCharCode(c)] = c + 32;
  for (let n = 0; n <= 9; n++) m['Digit' + n] = 0x30 + n;
  Object.assign(m, {
    Space: 0x20,
    Enter: 0xff0d,
    Tab: 0xff09,
    Backspace: 0xff08,
    Delete: 0xffff,
    Semicolon: 0x3b,
    Equal: 0x3d,
    Comma: 0x2c,
    Minus: 0x2d,
    Period: 0x2e,
    Slash: 0x2f,
    Quote: 0x27,
    BracketLeft: 0x5b,
    BracketRight: 0x5d,
    Backslash: 0x5c,
    Backquote: 0x60,
    ShiftLeft: 0xffe1,
    ShiftRight: 0xffe2,
    ControlLeft: 0xffe3,
    ControlRight: 0xffe4,
    CapsLock: 0xffe5,
    ArrowLeft: 0xff51,
    ArrowUp: 0xff52,
    ArrowRight: 0xff53,
    ArrowDown: 0xff54,
  });
  return m;
})();

function vcb01Send(bytes: number[]): void {
  const ws = VCB01D.ws;
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(new Uint8Array(bytes));
}

let vcb01KbdWired = false;
export function vcb01WireKeyboard(): void {
  if (vcb01KbdWired) return;
  vcb01KbdWired = true;
  const key = (e: KeyboardEvent, down: boolean) => {
    const cv = document.getElementById('vcb01-canvas');
    if (!cv || document.activeElement !== cv) return;
    const ks = VCB01_KEYSYM[e.code];
    if (!ks) return;
    e.preventDefault();
    vcb01Send([0x10, down ? 1 : 0, (ks >>> 24) & 255, (ks >> 16) & 255, (ks >> 8) & 255, ks & 255]);
  };
  document.addEventListener('keydown', (e) => key(e, true));
  document.addEventListener('keyup', (e) => key(e, false));
}

export function vcb01WireMouse(cv: HTMLCanvasElement): void {
  if (cv.dataset.wired) return;
  cv.dataset.wired = '1';
  cv.addEventListener('mousemove', (e) => {
    let dx = e.movementX | 0,
      dy = e.movementY | 0;
    if (dx || dy) {
      dx = Math.max(-32768, Math.min(32767, dx));
      dy = Math.max(-32768, Math.min(32767, dy));
      vcb01Send([0x11, (dx >> 8) & 255, dx & 255, (dy >> 8) & 255, dy & 255]);
    }
    if (VCB01D.w) {
      const ax = Math.max(0, Math.min(1023, Math.round((e.offsetX * VCB01D.w) / cv.clientWidth)));
      const ay = Math.max(0, Math.min(1023, Math.round((e.offsetY * VCB01D.h) / cv.clientHeight)));
      vcb01Send([0x13, (ax >> 8) & 255, ax & 255, (ay >> 8) & 255, ay & 255]);
    }
  });
  const btn = (e: MouseEvent) => {
    const b = ({ 0: 1, 1: 2, 2: 3 } as Record<number, number>)[e.button];
    if (!b) return;
    e.preventDefault();
    vcb01Send([0x12, b, e.type === 'mousedown' ? 1 : 0]);
  };
  cv.addEventListener('mousedown', (e) => {
    cv.focus();
    btn(e);
  });
  cv.addEventListener('mouseup', btn);
  cv.addEventListener('contextmenu', (e) => e.preventDefault());
}
