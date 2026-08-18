import { html } from '../html';
import { useState, useRef, useEffect } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { imageLabel } from '../lib/util';
import { pickImage } from '../lib/modals';

export function Toggle({ checked, onChange }: { checked: boolean; onChange: (v: boolean) => void }) {
  return html`<label class="toggle"><input type="checkbox" checked=${checked}
    onChange=${(e: Event) => onChange((e.target as HTMLInputElement).checked)} /><span class="slider"></span></label>`;
}

export function Chip({ cls, children }: { cls: string; children: ComponentChildren }) {
  return html`<span class=${'chip ' + cls}>${children}</span>`;
}

// The one indicator lamp used everywhere — topbar status, the control-panel
// DC ON / RUN lamps, the front-panel activity LEDs. One flat LED at one size on
// every surface. The default lens is red (the machine's own panel lamps); the
// topbar status row passes `green`.
// One panel lamp. `on` is normally a boolean, but a lamp that stands for a
// signal nobody is reading takes null: it draws as an empty seat rather than as
// a dark lens, because "not lit" and "not known" are different answers and only
// one of them may be shown as a measurement.
export function Led({
  on,
  green,
  title,
}: {
  on: boolean | null;
  green?: boolean;
  title?: string;
}) {
  const unknown = on === null;
  const state = unknown ? 'unknown' : on ? 'lit' : 'dark';
  return html`<span
    class=${'led' + (green ? ' green' : '') + (on ? ' on' : '') + (unknown ? ' unknown' : '')}
    role="img" aria-label=${(title ? title + ' — ' : '') + state}
    title=${title || null}></span>`;
}

// Shared image-assignment field: a button showing the drive's current medium
// that opens the image library and hands the chosen name (or "" to detach) to
// onPick. The detail pane wires it to a live or a staged assignment; the
// dashboard disk widgets can use the same component.
export function ImageField({
  drive,
  image,
  onPick,
}: {
  drive: string;
  image: string;
  onPick: (name: string) => void;
}) {
  const open = async () => {
    const name = await pickImage(
      'Image for ' + drive,
      'No image — leave the drive empty',
      image
    );
    if (name === null) return;
    onPick(name);
  };
  return html`<button class="imgfield mono" title=${image || 'no image'} onClick=${open}>
    ${image ? imageLabel(image) : html`<span class="muted">no image</span>`}</button>`;
}

// The file a device is programmed from, for a parameter the backend marks as
// naming one — a PROM card's socket. The same library browser the drives use,
// opened on the folder that kind of file belongs in; picking nothing empties
// the socket. The label is the file name, the full subpath its tooltip.
export function FileField({
  label,
  value,
  startDir,
  emptyLabel,
  onPick,
}: {
  label: string;
  value: string;
  startDir?: string;
  emptyLabel: string;
  onPick: (name: string) => void;
}) {
  const open = async () => {
    const name = await pickImage(label, emptyLabel, value, startDir);
    if (name === null) return;
    onPick(name);
  };
  return html`<button class="imgfield mono" title=${value || emptyLabel} onClick=${open}>
    ${value ? imageLabel(value) : html`<span class="muted">${emptyLabel}</span>`}</button>`;
}

const ARM_MS = 5000;
export function DangerButton({
  action,
  label,
  armLabel,
  onFire,
}: {
  action: string;
  label: string;
  armLabel: string;
  onFire: () => void;
}) {
  const [armed, setArmed] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  useEffect(() => () => clearTimeout(timer.current), []);
  useEffect(() => {
    const off = (e: MouseEvent) => {
      if (!(e.target as HTMLElement).closest('[data-danger="' + action + '"]')) setArmed(false);
    };
    document.addEventListener('click', off);
    return () => document.removeEventListener('click', off);
  }, [action]);
  const click = (e: MouseEvent) => {
    e.stopPropagation();
    if (!armed) {
      setArmed(true);
      clearTimeout(timer.current);
      timer.current = setTimeout(() => setArmed(false), ARM_MS);
      return;
    }
    setArmed(false);
    onFire();
  };
  return html`<button class=${'btn danger' + (armed ? ' armed' : '')} data-danger=${action} onClick=${click}>
    <span class="lbl"><span class="idle">${label}</span><span class="armed-lbl">${armLabel}</span></span></button>`;
}

export function DelButton({
  label,
  confirmLabel,
  onConfirm,
}: {
  label: string;
  confirmLabel: string;
  onConfirm: () => void;
}) {
  const [armed, setArmed] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  useEffect(() => () => clearTimeout(timer.current), []);
  return html`<button class="btn small" onClick=${() => {
    if (!armed) {
      setArmed(true);
      clearTimeout(timer.current);
      timer.current = setTimeout(() => setArmed(false), 3000);
      return;
    }
    setArmed(false);
    onConfirm();
  }}>${armed ? confirmLabel : label}</button>`;
}
