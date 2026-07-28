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

// Normalise a stored drive image reference to an images-root-relative subpath
// (e.g. "du/foo.dsk"). A drive's `image` param is stored as "images/<subpath>",
// but legacy values may be "./images/foo" or an absolute path that contains the
// images directory; strip whichever prefix is present.
export function imageSubpath(stored: string): string {
  if (!stored) return '';
  let s = stored;
  if (store.imagesDir && s.startsWith(store.imagesDir + '/')) s = s.slice(store.imagesDir.length + 1);
  if (s.startsWith('./')) s = s.slice(2);
  if (s.startsWith('images/')) s = s.slice('images/'.length);
  return s;
}

// Display label for a drive's medium: its subpath within the image tree.
export function imageLabel(stored: string): string {
  return imageSubpath(stored);
}

// The parent folder of a subpath ("" for a top-level entry).
export function parentDir(subpath: string): string {
  const i = subpath.lastIndexOf('/');
  return i < 0 ? '' : subpath.slice(0, i);
}

// The last path segment (file or folder name) of a subpath.
export function baseName(subpath: string): string {
  const i = subpath.lastIndexOf('/');
  return i < 0 ? subpath : subpath.slice(i + 1);
}

// A subpath under `base`, per-segment percent-encoded but keeping the slashes,
// so a route may hold folder separators without collapsing into one segment.
export function subURL(base: string, subpath: string): string {
  return (
    base +
    subpath
      .split('/')
      .filter(Boolean)
      .map(encodeURIComponent)
      .join('/')
  );
}

export const wsURL = (p: string): string =>
  (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + p;
