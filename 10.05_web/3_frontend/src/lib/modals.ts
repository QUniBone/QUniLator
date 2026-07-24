// Promise-based imperative overlays: confirm, image picker, first-run password.
import { apiJSON } from '../api';
import { toast } from './toast';
import { esc, humanSize, imageLabel } from './util';
import type { ImageInfo } from '../types';

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

export async function pickImage(
  title: string,
  emptyLabel: string,
  current: string
): Promise<string | null> {
  const images: ImageInfo[] = await fetch('/api/images')
    .then((r) => r.json())
    .catch(() => []);
  const now = imageLabel(current || '');
  const rows =
    images
      .map(
        (im) =>
          '<button class="pick-row' +
          (im.name === now ? ' current' : '') +
          '" data-pick-name="' +
          esc(im.name) +
          '">' +
          '<span class="mono">' +
          esc(im.name) +
          '</span>' +
          (im.name === now ? '<span class="chip ok">loaded</span>' : '') +
          '<span class="muted mono" style="font-size:var(--fs-0)">' +
          humanSize(im.size) +
          '</span></button>'
      )
      .join('') ||
    '<div class="muted" style="padding:8px">No images uploaded — add one on the Storage page.</div>';
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3>' +
      '<button class="modal-close" data-pick-close aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="pick-list">' +
      rows +
      '<button class="pick-row' +
      (now ? '' : ' current') +
      '" data-pick-name="">' +
      '<span class="muted">' +
      esc(emptyLabel) +
      '</span></button></div></div>';
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
      const row = target.closest('[data-pick-name]') as HTMLElement | null;
      if (row) done(row.dataset.pickName ?? '');
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
