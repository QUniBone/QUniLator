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

// Physical `e.code` -> X keysym. The emulated LK201 (`lk_code` in
// vcb01_input.cpp) translates the keysym to a make code, so these are the same
// X keysyms the X11 input path delivers. Covers the full main/editing/keypad
// LK201 layout; a driver that reads typing sees every key.
const VCB01_KEYSYM: Record<string, number> = (() => {
  const m: Record<string, number> = {};
  for (let c = 65; c <= 90; c++) m['Key' + String.fromCharCode(c)] = c + 32;
  for (let n = 0; n <= 9; n++) m['Digit' + n] = 0x30 + n;
  Object.assign(m, {
    Space: 0x20,
    Enter: 0xff0d,
    Tab: 0xff09,
    Backspace: 0xff08,
    Escape: 0xff1b,
    Delete: 0xffff,
    Insert: 0xff63,
    Home: 0xff50,
    End: 0xff57,
    PageUp: 0xff55,
    PageDown: 0xff56,
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
    IntlBackslash: 0x3c,
    ShiftLeft: 0xffe1,
    ShiftRight: 0xffe2,
    ControlLeft: 0xffe3,
    ControlRight: 0xffe4,
    CapsLock: 0xffe5,
    AltLeft: 0xffe9,
    AltRight: 0xffea,
    MetaLeft: 0xffe7,
    MetaRight: 0xffe8,
    ContextMenu: 0xff67,
    ArrowLeft: 0xff51,
    ArrowUp: 0xff52,
    ArrowRight: 0xff53,
    ArrowDown: 0xff54,
    // top-row function keys F1..F20 (LK201's F-row); browsers deliver F1..F12
    F1: 0xffbe,
    F2: 0xffbf,
    F3: 0xffc0,
    F4: 0xffc1,
    F5: 0xffc2,
    F6: 0xffc3,
    F7: 0xffc4,
    F8: 0xffc5,
    F9: 0xffc6,
    F10: 0xffc7,
    F11: 0xffc8,
    F12: 0xffc9,
    F13: 0xffca,
    F14: 0xffcb,
    F15: 0xffcc,
    F16: 0xffcd,
    F17: 0xffce,
    F18: 0xffcf,
    F19: 0xffd0,
    F20: 0xffd1,
    // numeric keypad
    NumLock: 0xff7f,
    NumpadDivide: 0xffaf,
    NumpadMultiply: 0xffaa,
    NumpadSubtract: 0xffad,
    NumpadAdd: 0xffab,
    NumpadEnter: 0xff8d,
    NumpadDecimal: 0xffae,
    NumpadComma: 0xffac,
    Numpad0: 0xffb0,
    Numpad1: 0xffb1,
    Numpad2: 0xffb2,
    Numpad3: 0xffb3,
    Numpad4: 0xffb4,
    Numpad5: 0xffb5,
    Numpad6: 0xffb6,
    Numpad7: 0xffb7,
    Numpad8: 0xffb8,
    Numpad9: 0xffb9,
  });
  return m;
})();

function vcb01Send(bytes: number[]): void {
  const ws = VCB01D.ws;
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(new Uint8Array(bytes));
}

function vcb01SendKey(keysym: number, down: boolean): void {
  vcb01Send([
    0x10,
    down ? 1 : 0,
    (keysym >>> 24) & 255,
    (keysym >> 16) & 255,
    (keysym >> 8) & 255,
    keysym & 255,
  ]);
}

// Keys currently held down on the canvas, physical `e.code` -> keysym. The
// emulated LK201 owns auto-repeat, so we report only press/release edges; this
// set lets a focus-loss handler release everything so nothing sticks.
const vcb01Down = new Map<string, number>();
const vcb01Unmapped = new Set<string>();

// LK201 all-up: release every key we believe is held. Called on any focus loss
// (window blur, tab hidden, canvas blur) because the matching keyup may never
// reach us once focus has left the canvas.
function vcb01ReleaseAll(): void {
  if (!vcb01Down.size) return;
  for (const ks of vcb01Down.values()) vcb01SendKey(ks, false);
  vcb01Down.clear();
}

let vcb01KbdWired = false;
export function vcb01WireKeyboard(): void {
  if (vcb01KbdWired) return;
  vcb01KbdWired = true;

  document.addEventListener('keydown', (e) => {
    const cv = document.getElementById('vcb01-canvas');
    if (!cv || document.activeElement !== cv) return;
    // The emulated LK201 generates auto-repeat itself; a browser auto-repeat is
    // one physical press, so send exactly one make and swallow the repeats.
    if (e.repeat) {
      e.preventDefault();
      return;
    }
    const ks = VCB01_KEYSYM[e.code];
    if (ks === undefined) {
      if (!vcb01Unmapped.has(e.code)) {
        vcb01Unmapped.add(e.code);
        console.warn(`vcb01: unmapped key ${e.code} (no VCB01_KEYSYM entry)`);
      }
      return;
    }
    e.preventDefault();
    vcb01Down.set(e.code, ks);
    vcb01SendKey(ks, true);
  });

  // Release on keyup for any key we are tracking, regardless of focus: once a
  // make has gone out, its break must follow or the guest holds a stuck key.
  document.addEventListener('keyup', (e) => {
    const ks = vcb01Down.get(e.code);
    if (ks === undefined) return;
    e.preventDefault();
    vcb01Down.delete(e.code);
    vcb01SendKey(ks, false);
  });

  // Focus can leave the canvas without a keyup ever reaching us (alt-tab, a
  // click elsewhere, switching tabs). Release everything held so the LK201 sees
  // the key go up.
  window.addEventListener('blur', vcb01ReleaseAll);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) vcb01ReleaseAll();
  });
  document.addEventListener('focusout', (e) => {
    if ((e.target as HTMLElement | null)?.id === 'vcb01-canvas') vcb01ReleaseAll();
  });
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
