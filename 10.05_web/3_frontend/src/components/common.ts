import { html } from '../html';
import { useState, useRef, useEffect } from 'preact/hooks';
import type { ComponentChildren } from 'preact';

export function Toggle({ checked, onChange }: { checked: boolean; onChange: (v: boolean) => void }) {
  return html`<label class="toggle"><input type="checkbox" checked=${checked}
    onChange=${(e: Event) => onChange((e.target as HTMLInputElement).checked)} /><span class="slider"></span></label>`;
}

export function Chip({ cls, children }: { cls: string; children: ComponentChildren }) {
  return html`<span class=${'chip ' + cls}>${children}</span>`;
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
