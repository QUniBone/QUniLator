// Promise-based imperative overlays: confirm, image picker, first-run password.
import { apiJSON } from '../api';
import { toast } from './toast';
import { esc, humanSize, imageSubpath, parentDir, baseName } from './util';
import type { ImageInfo, ImageListing } from '../types';

export function confirmModal(title: string, body: string, confirmLabel: string): Promise<boolean> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3>' +
      '<button class="modal-close" data-cm-no aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="card-body"><p class="muted" style="margin:0 0 16px; font-size:var(--fs-1)">' +
      body +
      '</p>' +
      '<div style="display:flex; gap:8px; justify-content:flex-end">' +
      '<button class="btn" data-cm-no>Cancel</button>' +
      '<button class="btn danger" data-cm-yes>' +
      esc(confirmLabel) +
      '</button></div></div></div>';
    const done = (v: boolean) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    function onKey(ev: KeyboardEvent) {
      if (ev.key === 'Escape') done(false);
    }
    host.addEventListener('click', (ev) => {
      const target = ev.target as HTMLElement;
      if (target === host || target.closest('[data-cm-no]')) done(false);
      else if (target.closest('[data-cm-yes]')) done(true);
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    (host.querySelector('[data-cm-yes]') as HTMLElement).focus();
  });
}

// A one-field text prompt (Save As, rename). Resolves the trimmed value, or
// null when cancelled.
//
// `select` says what the opened prompt offers to be typed over. 'all' is the
// whole value, which is what a rename or a Save As wants. 'stem' stops the
// selection before the last dot, for a suggested file name whose extension the
// medium implies: typing replaces the name and leaves the extension standing,
// and an operator who does mean to change it can still select it.
export function promptModal(
  title: string,
  label: string,
  initial: string,
  confirmLabel: string,
  select: 'all' | 'stem' = 'all'
): Promise<string | null> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3><button class="modal-close" data-pm-no aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="card-body"><div class="set-grid">' +
      '<label class="set-name" for="pm-in">' +
      esc(label) +
      '</label><input class="set-val mono" id="pm-in" type="text" value="' +
      esc(initial) +
      '"></div>' +
      '<div style="display:flex; gap:8px; justify-content:flex-end; margin-top:16px">' +
      '<button class="btn" data-pm-no>Cancel</button>' +
      '<button class="btn primary" data-pm-yes>' +
      esc(confirmLabel) +
      '</button></div></div></div>';
    const input = () => (host.querySelector('#pm-in') as HTMLInputElement).value.trim();
    const done = (v: string | null) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    const submit = () => {
      const v = input();
      if (v) done(v);
    };
    function onKey(ev: KeyboardEvent) {
      if (ev.key === 'Escape') done(null);
      else if (ev.key === 'Enter') submit();
    }
    host.addEventListener('click', (ev) => {
      const target = ev.target as HTMLElement;
      if (target === host || target.closest('[data-pm-no]')) done(null);
      else if (target.closest('[data-pm-yes]')) submit();
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    const el = host.querySelector('#pm-in') as HTMLInputElement;
    el.focus();
    // a leading dot is a hidden file's name, not an extension, so a value with
    // no dot past its first character has no extension to leave standing
    const dot = select === 'stem' ? initial.lastIndexOf('.') : -1;
    if (dot > 0) el.setSelectionRange(0, dot);
    else el.select();
  });
}

// Breadcrumb trail from the root to `cwd`, as clickable segments carrying the
// subpath each descends to (empty = root).
function crumbHTML(cwd: string): string {
  const segs = cwd ? cwd.split('/') : [];
  let acc = '';
  let out = '<button class="crumb" data-crumb="">root</button>';
  segs.forEach((seg) => {
    acc = acc ? acc + '/' + seg : seg;
    out += '<span class="crumb-sep">›</span><button class="crumb" data-crumb="' + esc(acc) + '">' + esc(seg) + '</button>';
  });
  return '<div class="pick-crumbs">' + out + '</div>';
}

// The folder-aware image picker. Walks the image tree in place; resolves the
// chosen file's subpath, "" for the empty option, or null when cancelled.
export async function pickImage(
  title: string,
  emptyLabel: string,
  current: string
): Promise<string | null> {
  const listing: ImageListing = await fetch('/api/images')
    .then((r) => r.json())
    .catch(() => ({ dirs: [], images: [] }));
  const dirs: string[] = listing.dirs || [];
  const images: ImageInfo[] = listing.images || [];
  const now = imageSubpath(current || '');
  // open in the folder that holds the current medium, so the list lands on it
  let cwd = now.includes('/') ? parentDir(now) : '';

  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3>' +
      '<button class="modal-close" data-pick-close aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="pick-body"></div></div>';
    const body = host.querySelector('.pick-body') as HTMLElement;

    const render = () => {
      const folders = dirs.filter((d) => parentDir(d) === cwd).sort();
      const files = images.filter((im) => im.dir === cwd).sort((a, b) => a.name.localeCompare(b.name));
      const folderRows = folders
        .map(
          (d) =>
            '<button class="pick-row folder" data-nav-dir="' +
            esc(d) +
            '"><span class="pick-icon">📁</span><span class="mono">' +
            esc(baseName(d)) +
            '</span><span class="muted" style="font-size:var(--fs-0)">folder</span></button>'
        )
        .join('');
      const fileRows = files
        .map(
          (im) =>
            '<button class="pick-row' +
            (im.path === now ? ' current' : '') +
            '" data-pick-name="' +
            esc(im.path) +
            '"><span class="pick-icon">💾</span><span class="mono">' +
            esc(im.name) +
            '</span>' +
            (im.path === now ? '<span class="chip ok">loaded</span>' : '') +
            '<span class="muted mono" style="font-size:var(--fs-0)">' +
            humanSize(im.size) +
            '</span></button>'
        )
        .join('');
      const empty =
        !folders.length && !files.length
          ? '<div class="muted" style="padding:8px">This folder is empty.</div>'
          : '';
      body.innerHTML =
        crumbHTML(cwd) +
        '<div class="pick-list">' +
        folderRows +
        fileRows +
        empty +
        '<button class="pick-row' +
        (now ? '' : ' current') +
        '" data-pick-empty><span class="pick-icon">∅</span><span class="muted">' +
        esc(emptyLabel) +
        '</span></button></div>';
    };
    render();

    const done = (v: string | null) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    function onKey(e2: KeyboardEvent) {
      if (e2.key === 'Escape') done(null);
    }
    host.addEventListener('click', (e) => {
      const target = e.target as HTMLElement;
      if (target === host || target.closest('[data-pick-close]')) {
        done(null);
        return;
      }
      const crumb = target.closest('[data-crumb]') as HTMLElement | null;
      if (crumb) {
        cwd = crumb.dataset.crumb ?? '';
        render();
        return;
      }
      const nav = target.closest('[data-nav-dir]') as HTMLElement | null;
      if (nav) {
        cwd = nav.dataset.navDir ?? '';
        render();
        return;
      }
      if (target.closest('[data-pick-empty]')) {
        done('');
        return;
      }
      const row = target.closest('[data-pick-name]') as HTMLElement | null;
      if (row) done(row.dataset.pickName ?? '');
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
  });
}

// Pick a device to add to a configuration, from the types not already in it.
// Resolves the chosen device handle, or null when cancelled.
export function pickDevice(
  title: string,
  options: { name: string; label: string; type: string }[]
): Promise<string | null> {
  const rows = options.length
    ? options
        .map(
          (o) =>
            '<button class="pick-row" data-pick-name="' +
            esc(o.name) +
            '"><span style="flex:1">' +
            esc(o.label) +
            '</span><span class="muted mono" style="font-size:var(--fs-0)">' +
            esc(o.name) +
            '</span></button>'
        )
        .join('')
    : '<div class="muted" style="padding:8px">Every device is already in this configuration.</div>';
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3><button class="modal-close" data-pick-close aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="pick-list">' +
      rows +
      '</div></div>';
    const done = (v: string | null) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape') done(null);
    }
    host.addEventListener('click', (e) => {
      const target = e.target as HTMLElement;
      if (target === host || target.closest('[data-pick-close]')) {
        done(null);
        return;
      }
      const row = target.closest('[data-pick-name]') as HTMLElement | null;
      if (row) done(row.dataset.pickName ?? null);
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
  });
}

function setPasswordModal(minLength: number): Promise<boolean> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>Set an admin password</h3></div>' +
      '<div class="card-body"><p class="muted" style="margin:0 0 16px; font-size:var(--fs-1)">' +
      'This interface controls the machine and is open to anyone who can reach it until a password ' +
      'is set. At least ' +
      minLength +
      ' characters. Any user name is accepted when the browser asks.</p>' +
      '<div class="set-grid">' +
      '<label class="set-name" for="pw1">Password</label>' +
      '<input class="set-val" id="pw1" type="password" autocomplete="new-password">' +
      '<label class="set-name" for="pw2">Repeat</label>' +
      '<input class="set-val" id="pw2" type="password" autocomplete="new-password"></div>' +
      '<p id="pw-err" class="muted" style="margin:12px 0 0; font-size:var(--fs-1); color:var(--err); min-height:1.2em"></p>' +
      '<div style="display:flex; gap:8px; justify-content:flex-end; margin-top:16px">' +
      '<button class="btn primary" data-pw-set>Set password</button></div></div></div>';
    const err = (msg: string) => {
      (host.querySelector('#pw-err') as HTMLElement).textContent = msg || '';
    };
    async function submit() {
      const p1 = (host.querySelector('#pw1') as HTMLInputElement).value;
      const p2 = (host.querySelector('#pw2') as HTMLInputElement).value;
      if (p1.length < minLength) return err('At least ' + minLength + ' characters.');
      if (p1 !== p2) return err('The two entries do not match.');
      err('');
      const res = await apiJSON<{ error?: string }>('/api/auth', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password: p1 }),
      }).catch(() => ({ ok: false, data: {} as { error?: string } }));
      if (!res.ok) return err(res.data.error || 'The password could not be set.');
      host.remove();
      toast('auth', 'Admin password set');
      resolve(true);
    }
    host.addEventListener('click', (ev) => {
      if ((ev.target as HTMLElement).closest('[data-pw-set]')) submit();
    });
    host.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') submit();
    });
    document.body.appendChild(host);
    (host.querySelector('#pw1') as HTMLElement).focus();
  });
}

export async function checkAuth(): Promise<void> {
  const r = await fetch('/api/auth');
  if (!r.ok) return;
  const auth = await r.json();
  if (auth.configured) return;
  await setPasswordModal(auth.min_length || 8);
}
