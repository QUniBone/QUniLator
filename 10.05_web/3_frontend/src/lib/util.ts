// small pure helpers shared across views
import { store } from '../store';

export const esc = (s: unknown): string =>
  String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');

export function octalStr(v: number, bitwidth?: number): string {
  return v.toString(8).padStart(Math.ceil((bitwidth || 16) / 3), '0');
}

export function humanSize(n: number): string {
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

export function imageLabel(path: string): string {
  if (!path) return '';
  const pfx = store.imagesDir + '/';
  if (store.imagesDir && path.startsWith(pfx)) {
    const rest = path.slice(pfx.length);
    if (rest.indexOf('/') === -1) return rest;
  }
  return path;
}

export const wsURL = (p: string): string =>
  (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + p;
