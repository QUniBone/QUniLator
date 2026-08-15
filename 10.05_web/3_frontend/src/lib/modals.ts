// Promise-based imperative overlays: confirm, image picker, first-run password.
import { apiJSON } from '../api';
import { toast } from './toast';
import { esc, humanSize, imageSubpath, octalStr, parentDir, baseName } from './util';
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

// A refusal the operator has to take note of: the machine would not do what
// they asked, and the reason it gave. It is dismissed by hand, so an operator
// who looked away still learns that the device is not in the machine — a
// message that fades on its own leaves them believing it went in.
export function alertModal(title: string, body: string): Promise<void> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3>' +
      '<button class="modal-close" data-am-ok aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="card-body"><p class="muted" style="margin:0 0 16px; font-size:var(--fs-1)">' +
      esc(body) +
      '</p>' +
      '<div style="display:flex; justify-content:flex-end">' +
      '<button class="btn" data-am-ok>OK</button></div></div></div>';
    const done = () => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve();
    };
    function onKey(ev: KeyboardEvent) {
      if (ev.key === 'Escape' || ev.key === 'Enter') done();
    }
    host.addEventListener('click', (ev) => {
      const target = ev.target as HTMLElement;
      if (target === host || target.closest('[data-am-ok]')) done();
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    (host.querySelector('[data-am-ok]') as HTMLElement).focus();
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

// ---- placing the memory card ----

export interface MemoryPlacement {
  startaddr: string; // octal, as the card's parameter reads
  size: string; // "2040 KB", as the card is described
}

// A byte count as the card describes itself: whole megabytes, else whole
// kilobytes, else a count of bytes.
function cardSize(bytes: number): string {
  if (bytes >= 1048576 && bytes % 1048576 === 0) return bytes / 1048576 + ' MB';
  if (bytes >= 1024 && bytes % 1024 === 0) return bytes / 1024 + ' KB';
  return bytes + ' bytes';
}

// Where the card goes, asked before it is put in the machine. A card is placed
// by a start address and a size, and the addresses left free are the ones above
// whatever memory the machine carries itself — so the fields open on the first
// free address and the rest of the space below the I/O page, and what the
// machine answers is on the page next to them.
//
// Sizing the machine's own memory sweeps the address space and takes the bus
// for the length of it, so the probe is a button the operator presses rather
// than something opening this dialog does.
export function pickPlacement(
  title: string,
  current: MemoryPlacement,
  confirmLabel: string
): Promise<MemoryPlacement | null> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3><button class="modal-close" data-mp-no aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="card-body">' +
      '<div class="mem-carries muted" style="display:flex; align-items:center; gap:10px; ' +
      'margin:0 0 14px; font-size:var(--fs-1)">' +
      '<span data-mp-carries style="flex:1">reading the map…</span>' +
      '<button class="btn small" data-mp-probe>Probe</button></div>' +
      '<div class="set-grid">' +
      '<label class="set-name" for="mp-start">start</label>' +
      '<input class="set-val mono" id="mp-start" type="text" value="' +
      esc(current.startaddr) +
      '">' +
      '<label class="set-name" for="mp-size">size</label>' +
      '<input class="set-val" id="mp-size" type="text" placeholder="256 KB" value="' +
      esc(current.size) +
      '"></div>' +
      '<div style="display:flex; gap:8px; justify-content:flex-end; margin-top:16px">' +
      '<button class="btn" data-mp-no>Cancel</button>' +
      '<button class="btn primary" data-mp-yes>' +
      esc(confirmLabel) +
      '</button></div></div></div>';

    const startEl = host.querySelector('#mp-start') as HTMLInputElement;
    const sizeEl = host.querySelector('#mp-size') as HTMLInputElement;
    const carries = host.querySelector('[data-mp-carries]') as HTMLElement;
    const probeBtn = host.querySelector('[data-mp-probe]') as HTMLButtonElement;

    // What the machine answers, and the placement that follows from it: the
    // first address its own memory leaves free, up to the I/O page.
    const readMap = (m: {
      addr_width: number;
      iopage_start: number;
      memory_limit: number;
      cpu_reserved_start: number | null;
      physical_end: number | null;
    }) => {
      if (m.physical_end === null) {
        carries.textContent = 'the machine’s own memory has not been sized';
        return;
      }
      const free = m.physical_end + 2;
      // The card stops below the memory the CPU module answers on-module, not
      // at the I/O page: the board cannot see that claim over the bus, so a
      // placement filling the space to the page is written by DMA and read
      // back as ROM, and the CPU's own RAM test is what reports it.
      const limit = m.memory_limit || m.iopage_start;
      carries.textContent =
        'the machine answers ' +
        octalStr(0, m.addr_width) +
        '..' +
        octalStr(m.physical_end, m.addr_width) +
        ' — ' +
        cardSize(free) +
        (m.cpu_reserved_start
          ? '; the CPU answers ' + octalStr(m.cpu_reserved_start, m.addr_width) + ' up itself'
          : '');
      startEl.value = octalStr(free, m.addr_width);
      sizeEl.value = free < limit ? cardSize(limit - free) : '0 bytes';
    };

    apiJSON<{
      addr_width: number;
      iopage_start: number;
      memory_limit: number;
      cpu_reserved_start: number | null;
      physical_end: number | null;
    }>('/api/memory/map')
      .then((r) => {
        if (r.ok) readMap(r.data);
      })
      .catch(() => {});

    const probe = async () => {
      probeBtn.disabled = true;
      carries.textContent = 'sizing the machine’s memory…';
      const r = await apiJSON<{ error?: string }>('/api/memory/probe', { method: 'POST' });
      probeBtn.disabled = false;
      if (!r.ok) {
        carries.textContent = r.data.error || 'the probe was refused';
        return;
      }
      const m = await apiJSON<{
        addr_width: number;
        iopage_start: number;
        memory_limit: number;
        cpu_reserved_start: number | null;
        physical_end: number | null;
      }>('/api/memory/map');
      if (m.ok) readMap(m.data);
    };

    const done = (v: MemoryPlacement | null) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    const submit = () => {
      const startaddr = startEl.value.trim();
      const size = sizeEl.value.trim();
      if (startaddr && size) done({ startaddr, size });
    };
    function onKey(ev: KeyboardEvent) {
      if (ev.key === 'Escape') done(null);
      else if (ev.key === 'Enter') submit();
    }
    host.addEventListener('click', (ev) => {
      const target = ev.target as HTMLElement;
      if (target === host || target.closest('[data-mp-no]')) done(null);
      else if (target.closest('[data-mp-probe]')) probe();
      else if (target.closest('[data-mp-yes]')) submit();
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    startEl.focus();
    startEl.select();
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
  current: string,
  // Where the browser opens when the parameter holds nothing yet: the folder
  // that kind of file belongs in (a PROM card opens on roms/). With a value
  // set, the folder holding it wins — the list lands on what is loaded.
  startDir?: string
): Promise<string | null> {
  const listing: ImageListing = await fetch('/api/images')
    .then((r) => r.json())
    .catch(() => ({ dirs: [], images: [] }));
  const dirs: string[] = listing.dirs || [];
  const images: ImageInfo[] = listing.images || [];
  const now = imageSubpath(current || '');
  // open in the folder that holds the current medium, so the list lands on it
  let cwd = now.includes('/') ? parentDir(now) : startDir || '';

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

// The order the device groups stand in, which is the order a backplane is read:
// the controllers a machine is built around first, the odds and ends last. A
// category not named here follows them, so a device type added later still has
// a place.
const DEVICE_CATEGORIES = ['controller', 'memory', 'serial', 'network', 'video', 'clock', 'other'];

function categoryRank(c: string): number {
  const i = DEVICE_CATEGORIES.indexOf(c);
  return i < 0 ? DEVICE_CATEGORIES.length : i;
}

// Pick a device to add to a configuration, from the types not already in it.
// The list is grouped by category and sorted by the label the operator reads,
// so it opens the same way every time; a search field narrows it on label and
// handle, and the body scrolls, so a machine offering more devices than the
// window is tall still reaches its last row. Resolves the chosen device handle,
// or null when cancelled.
export function pickDevice(
  title: string,
  options: { name: string; label: string; type: string; category?: string }[],
  // What the list holds, for the search field and the two empty states. The
  // picker is the same widget whatever it offers — devices, blank media, the
  // ROMs the package ships — so only its wording changes.
  texts?: { noun?: string; empty?: string }
): Promise<string | null> {
  const noun = texts?.noun || 'device';
  const sorted = options.slice().sort((a, b) => {
    const ca = a.category || 'other',
      cb = b.category || 'other';
    if (categoryRank(ca) !== categoryRank(cb)) return categoryRank(ca) - categoryRank(cb);
    if (ca !== cb) return ca.localeCompare(cb);
    return a.label.localeCompare(b.label);
  });
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>' +
      esc(title) +
      '</h3><button class="modal-close" data-pick-close aria-label="Close" title="Close">&times;</button></div>' +
      (options.length
        ? '<div class="pick-head"><input class="pick-search" type="text" ' +
          'placeholder="Search ' + esc(noun) + 's" aria-label="Search ' + esc(noun) + 's"></div>'
        : '') +
      '<div class="pick-body"><div class="pick-list"></div></div></div>';
    const list = host.querySelector('.pick-list') as HTMLElement;
    const search = host.querySelector('.pick-search') as HTMLInputElement | null;

    // the rows the query leaves, and which of them the keyboard is on
    let shown = sorted;
    let cursor = 0;

    const render = () => {
      const q = (search ? search.value : '').trim().toLowerCase();
      shown = q
        ? sorted.filter((o) => (o.label + ' ' + o.name).toLowerCase().includes(q))
        : sorted;
      cursor = Math.min(cursor, Math.max(0, shown.length - 1));
      if (!options.length) {
        list.innerHTML =
          '<div class="muted" style="padding:8px">' +
          esc(texts?.empty || 'Every device is already in this configuration.') +
          '</div>';
        return;
      }
      if (!shown.length) {
        list.innerHTML =
          '<div class="muted" style="padding:8px">No ' + esc(noun) + ' matches.</div>';
        return;
      }
      // A narrowed list is one run of matches: the groups it would be cut into
      // say nothing the rows do not, and an empty heading would stand over
      // nothing. So the headings are for the whole list only.
      let group = '';
      list.innerHTML = shown
        .map((o, i) => {
          let head = '';
          const cat = o.category || 'other';
          if (!q && cat !== group) {
            group = cat;
            head = '<div class="pick-group">' + esc(cat) + '</div>';
          }
          return (
            head +
            '<button class="pick-row' +
            (i === cursor ? ' sel' : '') +
            '" data-pick-name="' +
            esc(o.name) +
            '"><span style="flex:1">' +
            esc(o.label) +
            '</span><span class="muted mono" style="font-size:var(--fs-0)">' +
            esc(o.name) +
            '</span></button>'
          );
        })
        .join('');
      const sel = list.querySelector('.pick-row.sel');
      if (sel) sel.scrollIntoView({ block: 'nearest' });
    };

    const done = (v: string | null) => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve(v);
    };
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape') {
        done(null);
      } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        if (!shown.length) return;
        e.preventDefault();
        cursor = (cursor + (e.key === 'ArrowDown' ? 1 : shown.length - 1)) % shown.length;
        render();
      } else if (e.key === 'Enter') {
        if (shown.length) done(shown[cursor].name);
      }
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
    if (search) search.addEventListener('input', render);
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    render();
    if (search) search.focus();
  });
}

// One DNS label: letters, digits and inner hyphens, at most 63 characters.
// <name>.local, the DNS-SD entry, the DHCP lease and the login banner all
// follow it, so several QUniLators on a network are told apart by it.
export const HOST_NAME_RULE = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;
export const HOST_NAME_REFUSAL =
  'A host name is letters, digits and inner hyphens, up to 63 characters.';

// The types OpenSSH offers, which is what a .pub file opens with.
export const SSH_KEY_RULE =
  /^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp(256|384|521)|sk-ssh-ed25519@openssh\.com|sk-ecdsa-sha2-nistp256@openssh\.com) +[A-Za-z0-9+/=]{16,}( .*)?$/;
export const SSH_KEY_REFUSAL =
  'That is not an ssh public key — paste one line, the contents of a .pub file.';

// The operator's account: the browser's basic-auth prompt, the SMB, FTP and
// SFTP shares and an ssh login all take the same name and password.
export const USER_NAME_RULE = /^[a-z_][a-z0-9_-]{0,31}$/;
export const USER_NAME_HELP =
  'A user name is 1 to 32 characters: a lower case letter or underscore, then lower case ' +
  'letters, digits, underscores and hyphens.';
// said where the rule is already on the page, so it points at it rather than repeating it
export const USER_NAME_REFUSAL = 'The user name does not follow that rule.';

// What to do with the board now that it has a name and an account. It is shown
// once, at the end of the first-run dialog, because this is the moment the two
// things an operator needs are both settled and neither is anywhere on screen:
// the page was opened on an address handed out by DHCP, which the next lease
// may change, and <name>.local is what follows the board instead. The ssh line
// is the same account, said here because the dialog is where the key was given.
function setupDoneModal(user: string, board: string): Promise<void> {
  const dotLocal = board + '.local';
  const url = 'http://' + dotLocal + '/';
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>This QUniLator is ' +
      esc(board) +
      '</h3>' +
      '<button class="modal-close" data-sd-ok aria-label="Close" title="Close">&times;</button></div>' +
      '<div class="card-body"><p class="muted" style="margin:0 0 12px; font-size:var(--fs-1)">' +
      'Reach it by that name rather than by the address this page was opened on — the address ' +
      'comes from the network and can change, the name stays with the board:</p>' +
      '<p style="margin:0 0 16px"><a class="mono" style="color:var(--accent)" href="' +
      esc(url) +
      '">' +
      esc(url) +
      '</a></p>' +
      '<p class="muted" style="margin:0 0 12px; font-size:var(--fs-1)">' +
      'The same account reaches a shell on the board, with sudo:</p>' +
      '<p class="mono" style="margin:0 0 16px">ssh ' +
      esc(user) +
      '@' +
      esc(dotLocal) +
      '</p>' +
      '<p class="muted" style="margin:0; font-size:var(--fs-1)">' +
      'The browser asks for the name and password now.</p>' +
      '<div style="display:flex; justify-content:flex-end; margin-top:16px">' +
      '<button class="btn primary" data-sd-ok>Got it</button></div></div></div>';
    const done = () => {
      host.remove();
      document.removeEventListener('keydown', onKey);
      resolve();
    };
    function onKey(ev: KeyboardEvent) {
      if (ev.key === 'Escape' || ev.key === 'Enter') done();
    }
    host.addEventListener('click', (ev) => {
      const target = ev.target as HTMLElement;
      // the link is the point of the dialog: let it open, and leave the dialog
      // standing behind it rather than closing on the way out
      if (target.closest('a')) return;
      if (target === host || target.closest('[data-sd-ok]')) done();
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(host);
    (host.querySelector('[data-sd-ok]') as HTMLElement).focus();
  });
}

function setCredentialsModal(minLength: number, hostname: string): Promise<boolean> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    host.className = 'modal-overlay';
    host.innerHTML =
      '<div class="card modal-card"><div class="card-head"><h3>Set this QUniLator up</h3></div>' +
      '<div class="card-body"><p class="muted" style="margin:0 0 16px; font-size:var(--fs-1)">' +
      'This interface controls the machine, and until an account exists anyone who can reach it ' +
      'can drive it. The name and password set here make that one account: the browser asks for ' +
      'it, and so do the SMB, FTP and SFTP shares of the image library and an ssh login. The ' +
      'password is at least ' +
      minLength +
      ' characters.</p>' +
      '<div class="set-grid">' +
      '<label class="set-name" for="pw-user">User name</label>' +
      '<input class="set-val" id="pw-user" type="text" autocomplete="username" ' +
      'autocapitalize="off" spellcheck="false">' +
      '<label class="set-name" for="pw1">Password</label>' +
      '<input class="set-val" id="pw1" type="password" autocomplete="new-password">' +
      '<label class="set-name" for="pw2">Repeat</label>' +
      '<input class="set-val" id="pw2" type="password" autocomplete="new-password">' +
      '<label class="set-name" for="pw-host">Host name</label>' +
      '<input class="set-val" id="pw-host" type="text" autocapitalize="off" ' +
      'spellcheck="false" value="' +
      esc(hostname) +
      '">' +
      '<label class="set-name" for="pw-key">SSH key</label>' +
      '<textarea class="set-val mono" id="pw-key" rows="3" autocapitalize="off" ' +
      'spellcheck="false" placeholder="ssh-ed25519 AAAA… you@workstation"></textarea>' +
      '</div>' +
      '<p class="muted" style="margin:12px 0 0; font-size:var(--fs-1)">' +
      esc(USER_NAME_HELP) +
      ' The host name is the name this QUniLator has on the network, as ' +
      '<span class="mono">&lt;name&gt;.local</span>. An ssh public key is optional: give one and ' +
      'the same account reaches a shell, with sudo.</p>' +
      '<p id="pw-err" class="muted" style="margin:12px 0 0; font-size:var(--fs-1); color:var(--err); min-height:1.2em"></p>' +
      '<div style="display:flex; gap:8px; justify-content:flex-end; margin-top:16px">' +
      '<button class="btn primary" data-pw-set><span data-pw-label>Set up</span></button>' +
      '</div></div></div>';
    const err = (msg: string) => {
      (host.querySelector('#pw-err') as HTMLElement).textContent = msg || '';
    };
    // Creating the account, setting the host name and installing the key take
    // a few seconds, so the button says the work is under way and the dialog
    // takes no second submission while it runs.
    let busy = false;
    const setBusy = (on: boolean) => {
      busy = on;
      const btn = host.querySelector('[data-pw-set]') as HTMLButtonElement;
      btn.disabled = on;
      (host.querySelector('[data-pw-label]') as HTMLElement).textContent = on
        ? 'Setting up…'
        : 'Set up';
      btn.querySelector('.btn-spinner')?.remove();
      if (on) {
        const s = document.createElement('span');
        s.className = 'btn-spinner';
        s.setAttribute('aria-hidden', 'true');
        btn.prepend(s);
      }
      host.querySelectorAll('input, textarea').forEach((el) => {
        (el as HTMLInputElement).disabled = on;
      });
    };
    async function submit() {
      if (busy) return;
      const user = (host.querySelector('#pw-user') as HTMLInputElement).value.trim();
      const p1 = (host.querySelector('#pw1') as HTMLInputElement).value;
      const p2 = (host.querySelector('#pw2') as HTMLInputElement).value;
      const board = (host.querySelector('#pw-host') as HTMLInputElement).value.trim();
      const key = (host.querySelector('#pw-key') as HTMLTextAreaElement).value.trim();
      if (!USER_NAME_RULE.test(user)) return err(USER_NAME_REFUSAL);
      if (p1.length < minLength) return err('At least ' + minLength + ' characters.');
      if (p1 !== p2) return err('The two entries do not match.');
      if (board && !HOST_NAME_RULE.test(board)) return err(HOST_NAME_REFUSAL);
      if (key && !SSH_KEY_RULE.test(key)) return err(SSH_KEY_REFUSAL);
      err('');
      setBusy(true);
      // One request: the key belongs to the account these credentials create,
      // and this is the last call the browser makes before it has to
      // authenticate.
      const res = await apiJSON<{ error?: string; warnings?: string[] }>('/api/auth', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user, password: p1, hostname: board, ssh_key: key }),
      }).catch(() => ({ ok: false, data: {} as { error?: string; warnings?: string[] } }));
      if (!res.ok) {
        setBusy(false);
        return err(res.data.error || 'The credentials could not be set.');
      }
      host.remove();
      const warnings = res.data.warnings || [];
      const warns = warnings.join('; ');
      toast('auth', warns || 'Credentials set for ' + user);
      // The name in force is the one that was asked for, unless the field was
      // left empty (which keeps the board's own) or the backend refused it and
      // said so in a warning — in either of those the old name still stands.
      const refused = warnings.some((w) => w.indexOf('host name') >= 0);
      const name = board && !refused ? board : hostname;
      if (name) await setupDoneModal(user, name);
      resolve(true);
    }
    host.addEventListener('click', (ev) => {
      if ((ev.target as HTMLElement).closest('[data-pw-set]')) submit();
    });
    host.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') submit();
    });
    document.body.appendChild(host);
    (host.querySelector('#pw-user') as HTMLElement).focus();
  });
}

export async function checkAuth(): Promise<void> {
  const r = await fetch('/api/auth');
  if (!r.ok) return;
  const auth = await r.json();
  if (auth.configured) return;
  // the name the board carries now, offered as what to keep or change
  const board = await fetch('/api/hostname')
    .then((h) => (h.ok ? h.json() : { hostname: '' }))
    .catch(() => ({ hostname: '' }));
  await setCredentialsModal(auth.min_length || 8, board.hostname || '');
}
