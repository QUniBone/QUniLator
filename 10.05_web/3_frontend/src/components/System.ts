// The System pages: who reaches this QUniLator, what it is called, which of the
// BeagleBone's ports carry a login, what it has recorded, and updating it. One
// function per page, reached from the sub-menu the sidebar opens under System.
//
// The two update sections are kept apart because they promise different things.
// The emulator update is reversible - QUniLator watches the new version come up
// and puts the old one back if it does not - and it stops the emulated machine,
// so it says so and asks for a confirmation. The operating-system upgrade cannot
// be undone by apt at all, and leaves the machine running, so it says that
// instead.
import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import { useStore } from '../store';
import { apiJSON, liveControl, listRecordings, deleteRecording, type Recording } from '../api';
import {
  confirmModal,
  HOST_NAME_REFUSAL,
  HOST_NAME_RULE,
  SSH_KEY_REFUSAL,
  SSH_KEY_RULE,
  USER_NAME_HELP,
  USER_NAME_REFUSAL,
  USER_NAME_RULE,
} from '../lib/modals';
import { toast } from '../lib/toast';
import { bundleVersion } from '../lib/version';
import {
  refreshUpdate,
  checkForUpdate,
  fetchChangelog,
  parseChangelog,
  dismissUpdate,
  discardUpdateFile,
  installUpdate,
  uploadUpdateFile,
  upgradeOs,
  updateAvailable,
  updateRunning,
} from '../lib/update';
import type { Release } from '../lib/update';
import type { UpdateProgress, UpdateStatus } from '../types';

// "2026-07-30T09:12:00Z" as something a person reads, in local time
function when(iso: string): string {
  if (!iso) return 'never';
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  return d.toLocaleString();
}

function StateChip({ u }: { u: UpdateStatus }) {
  if (u.state === 'checking') return html`<span class="chip warn">checking</span>`;
  if (updateRunning(u)) return html`<span class="chip warn">${u.state}</span>`;
  if (u.state === 'failed') return html`<span class="chip err">failed</span>`;
  if (u.state === 'rolled-back') return html`<span class="chip err">rolled back</span>`;
  if (u.state === 'ahead')
    return html`<span class="chip out" title="the repository offers an older version than this QUniLator runs">ahead of the repository</span>`;
  if (updateAvailable(u)) return html`<span class="chip warn">update available</span>`;
  if (!u.source_configured) return html`<span class="chip off">no update source</span>`;
  return html`<span class="chip ok">up to date</span>`;
}

function releaseDate(rfc2822: string): string {
  const d = new Date(rfc2822);
  return isNaN(d.getTime()) ? rfc2822 : d.toLocaleDateString();
}

// The releases since the installed version, fetched when the operator asks for
// them: reading the changelog makes QUniLator download the candidate, which is
// also the staging step an install wants, so it is not fetched on every page
// view. One row per release, its title showing and its note a click away. With
// `local` it reads the staged update file's package, which may carry nothing
// newer than what is installed.
function Changelog({ open, local = false }: { open: boolean; local?: boolean }) {
  const [text, setText] = useState<string | null>(null);
  useEffect(() => {
    if (!open || text !== null) return;
    setText('');
    fetchChangelog(local).then((t) =>
      setText(
        t ||
          (local
            ? 'The update file carries no release newer than the installed version.'
            : 'The changelog could not be read.')
      )
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);
  if (!open) return null;
  if (text === '') return html`<p class="muted">Fetching the changelog…</p>`;
  const releases: Release[] = parseChangelog(text || '');
  if (!releases.length) return html`<pre class="upd-changelog">${text}</pre>`;
  return html`<div class="upd-releases">
    <p class="muted">${releases.length === 1
      ? 'One release since the installed version.'
      : `${releases.length} releases since the installed version.`} Click one for its full note.</p>
    ${releases.map(
      (r) => html`<details class="upd-release" key=${r.version}>
        <summary>
          <span class="pill mono">${r.version}</span>
          <span class="upd-release-title">${r.title}</span>
          <span class="muted upd-release-date">${releaseDate(r.date)}</span>
        </summary>
        <pre class="upd-changelog">${r.note}</pre>
      </details>`
    )}
  </div>`;
}

// What an install costs, said before the button rather than after: the machine
// stops, its configuration is the startup one again, and unsaved edits are lost.
// `local` offers the staged update file's version rather than the repository's
// candidate, and discards the file where the repository's offer is dismissed.
function InstallDialog({
  u,
  version,
  local = false,
  onDone,
}: {
  u: UpdateStatus;
  version: string;
  local?: boolean;
  onDone: () => void;
}) {
  const s = useStore();
  const [confirmed, setConfirmed] = useState(false);
  const [showLog, setShowLog] = useState(true);
  const running = !!s.hw.powered && !s.bus.halted;
  const modified = s.configModified === true;
  const again = version === u.installed;
  // A check refreshes the package lists the changelog download reads, so the
  // changelog waits for it; keyed by version, so a check that finds a newer one
  // fetches again.
  return html`<div class="upd-install">
    ${local
      ? html`<p class="muted">The update file carries <span class="mono">${version}</span>${again
          ? html` — the version this QUniLator runs, so installing it installs it again`
          : null}. Its signature checks out.</p>`
      : null}
    <${Changelog} key=${(local ? 'file:' : '') + version} local=${local}
      open=${showLog && (local || u.state !== 'checking')} />
    ${!showLog
      ? html`<button class="btn small" onClick=${() => setShowLog(true)}>Show what changed</button>`
      : null}
    <ul class="upd-costs">
      <li><strong>The emulated machine stops.</strong> The device set is shut down in
        order, so the disk images are flushed — but a running Unix or RSX has its
        buffer cache cut. ${
          running
            ? html`The machine is <strong>running</strong> now.
              <button class="btn small" onClick=${() => liveControl('halt', 'machine halted')}>Halt the machine</button>`
            : html`The machine is halted.`
        }</li>
      <li>QUniLator comes back up in its <strong>startup configuration</strong>${
        s.configCurrent ? html` — currently <span class="mono">${s.configCurrent}</span>` : null
      }, which is not necessarily the one it is running.${
        modified
          ? html` The live configuration has <strong>unsaved changes</strong>; ${' '}<a href="/config">save them first</a> if you want them back.`
          : null
      }</li>
      <li>The interface reconnects on its own, in about half a minute.</li>
    </ul>
    ${
      running
        ? html`<label class="radio upd-confirm"><input type="checkbox" checked=${confirmed}
            onChange=${(e: Event) => setConfirmed((e.target as HTMLInputElement).checked)} />
            Install anyway, with the machine running</label>`
        : null
    }
    <div class="upd-buttons">
      <button class="btn primary" disabled=${running && !confirmed}
        onClick=${() => installUpdate(version, local)}>${again ? 'Reinstall' : 'Install'} ${version}</button>
      <button class="btn" onClick=${async () => {
        if (local) await discardUpdateFile();
        else await dismissUpdate(version);
        onDone();
      }}>${local ? 'Discard the file' : 'Dismiss'}</button>
    </div>
  </div>`;
}

// The way to update a QUniLator that cannot reach the repository: an update file
// fetched on another computer and uploaded here. QUniLator checks it before it
// answers, so a file that is not signed by the repository, or is for the other
// board, is refused here rather than at install time; the reason is the
// updater's, shown with the page's other update errors.
function UpdateFileRow({ u, busy }: { u: UpdateStatus; busy: boolean }) {
  const [checking, setChecking] = useState(false);
  const input = useRef<HTMLInputElement>(null);
  const staged = u.local?.version || '';
  const pick = async (e: Event) => {
    const el = e.target as HTMLInputElement;
    const file = el.files && el.files[0];
    el.value = '';
    if (!file) return;
    setChecking(true);
    const res = await uploadUpdateFile(file);
    setChecking(false);
    toast('POST /api/update/upload', res.ok ? 'update file verified' : res.error || 'the file was refused');
  };
  return html`
    <div class="set-name">Update file</div>
    <div class="set-val">
      ${staged ? html`<span class="pill mono">${staged}</span>` : null}
      <input type="file" accept=".tar" ref=${input} hidden onChange=${pick} />
      <button class="btn small" disabled=${busy || checking}
        onClick=${() => input.current?.click()}>${checking ? 'Checking the file…' : 'Upload…'}</button>
    </div>
    <div class="set-info">For a QUniLator that cannot reach the repository. On another computer,
      download <span class="mono">${u.package + "_<version>_armhf.update.tar"}</span> from the${' '}
      <a href="https://github.com/QUniBone/QUniLator/releases" target="_blank" rel="noopener">release
      page</a> and upload it here. It is checked against the repository's signature before
      anything is offered for installing.</div>`;
}

// mm:ss, or h:mm:ss past an hour
function duration(ms: number): string {
  const t = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(t / 3600);
  const m = Math.floor((t % 3600) / 60);
  const sec = String(t % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

const STEP_TEXT: Record<UpdateProgress['step'], string> = {
  refreshing: 'Refreshing the package lists',
  starting: 'Working out what to upgrade',
  downloading: 'Downloading',
  installing: 'Installing',
};

// A running operating-system upgrade: apt's step and percentage, how long it
// has been going, and the tail of its output, which moves as long as apt does.
// The status is rewritten every two seconds while apt runs, so a log that has
// stopped moving for minutes is the sign of a stuck upgrade.
function OsProgress({ u }: { u: UpdateStatus }) {
  const p = u.progress;
  const [now, setNow] = useState(Date.now());
  const logRef = useRef<HTMLPreElement>(null);
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  const journal = u.journal || [];
  useEffect(() => {
    const el = logRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [journal.length, journal[journal.length - 1]]);
  const started = p?.started_at ? new Date(p.started_at).getTime() : NaN;
  const pct = p && p.percent !== null ? Math.min(100, Math.max(0, p.percent)) : null;
  return html`<div class="upd-progress">
    <p class="upd-phase">${p ? STEP_TEXT[p.step] || p.step : 'Upgrading'}${
      pct !== null ? html` — ${pct}%` : null
    }${p && p.message ? html`<span class="muted"> · ${p.message}</span>` : null}</p>
    <div class=${'upd-bar' + (pct !== null ? ' determinate' : '')}>
      <div class="upd-bar-fill" style=${pct !== null ? `width:${pct}%` : ''}></div>
    </div>
    <p class="muted upd-elapsed">${
      isNaN(started) ? '' : duration(now - started) + ' elapsed. '
    }An upgrade on a BeagleBone takes several minutes; the emulated machine keeps running.</p>
    ${journal.length ? html`<pre class="upd-changelog upd-live" ref=${logRef}>${journal.join('\n')}</pre>` : null}
  </div>`;
}

// How the last operating-system upgrade went, with its output a click away.
function OsLast({ u }: { u: UpdateStatus }) {
  const l = u.os_last;
  const [open, setOpen] = useState(false);
  if (!l || !l.state) return null;
  const took =
    l.started_at && l.at ? new Date(l.at).getTime() - new Date(l.started_at).getTime() : NaN;
  return html`<div class="upd-last">
    <div class="set-name">Last upgrade</div>
    <p class="muted">${l.state === 'done' ? 'Upgraded' : 'Failed'} — ${when(l.at || '')}${
      isNaN(took) ? '' : `, in ${duration(took)}`
    }.${
      l.journal && l.journal.length
        ? html` <button class="btn small" onClick=${() => setOpen(!open)}>${open ? 'Hide' : 'Show'} the log</button>`
        : null
    }</p>
    ${l.state === 'failed' && l.error ? html`<p class="upd-warn">${l.error}</p>` : null}
    ${open && l.journal ? html`<pre class="upd-changelog">${l.journal.join('\n')}</pre>` : null}
  </div>`;
}

function OsRow({ u }: { u: UpdateStatus }) {
  const os = u.os || { count: 0, packages: [], held_back: [], reboot_required: false };
  const [open, setOpen] = useState(false);
  const busy = updateRunning(u);
  if (u.state === 'os-upgrading')
    return html`<div class="card" style="max-width:720px; margin-top:14px">
      <div class="card-head"><h3>Operating system</h3><span class="chip warn">upgrading</span></div>
      <div class="card-body"><${OsProgress} u=${u} /></div></div>`;
  return html`<div class="card" style="max-width:720px; margin-top:14px">
    <div class="card-head"><h3>Operating system</h3>
      ${os.count ? html`<span class="chip warn">${os.count} to upgrade</span>` : html`<span class="chip ok">up to date</span>`}</div>
    <div class="card-body">
      ${
        os.count
          ? html`<p class="muted" style="margin:0 0 10px">${os.count} other package${
              os.count === 1 ? ' has' : 's have'
            } updates.
            <button class="btn small" onClick=${() => setOpen(!open)}>${open ? 'Hide' : 'Show'} the list</button></p>
            ${
              open
                ? html`<pre class="upd-changelog">${os.packages
                    .map((p) => p.name + '  ' + p.from + ' → ' + p.to)
                    .join('\n')}</pre>`
                : null
            }`
          : html`<p class="muted" style="margin:0 0 10px">Nothing else on the BeagleBone has an update.</p>`
      }
      ${
        os.held_back && os.held_back.length
          ? html`<p class="muted" style="margin:0 0 10px">Held back: <span class="mono">${os.held_back.join(', ')}</span>.
            The kernel is held deliberately — the cape port depends on it.</p>`
          : null
      }
      ${
        os.kept_back && os.kept_back.length
          ? html`<p class="muted" style="margin:0 0 10px">Kept back: <span class="mono">${os.kept_back.join(', ')}</span>.
            ${os.kept_back.length === 1 ? 'Its new version needs' : 'Their new versions need'} packages
            that are not installed yet, such as a new kernel, which this upgrade does not add.</p>`
          : null
      }
      ${
        os.reboot_required
          ? html`<p class="upd-warn">A reboot is required to finish an earlier upgrade.
            Nothing here reboots the BeagleBone; a reboot stops the emulated machine, so it is
            your call when.</p>`
          : null
      }
      ${
        os.count
          ? html`<p class="upd-warn"><strong>This cannot be undone.</strong> apt cannot reverse an
              upgrade, so an operating-system upgrade that breaks it is an ssh session or a
              reflash — unlike the emulator update, which puts the old version back by itself. The
              emulator package is held back, so the emulated machine keeps running throughout.</p>
            <button class="btn" disabled=${busy} onClick=${async () => {
              const ok = await confirmModal(
                'Upgrade the operating system',
                'This upgrades ' +
                  os.count +
                  " of the BeagleBone's packages. It cannot be undone: apt has no way to " +
                  'reverse an upgrade. The emulated machine keeps running, and the BeagleBone is ' +
                  'not rebooted.',
                'Upgrade'
              );
              if (ok) await upgradeOs();
            }}>Upgrade the operating system</button>`
          : null
      }
      <${OsLast} u=${u} />
    </div></div>`;
}

type AuthStatus = {
  configured: boolean;
  user: string;
  min_length: number;
};

// The operator's account: one name and one password, which the web interface,
// the file shares and ssh all take. It is created when this QUniLator is set up
// — when the SD card is prepared, or in the first-run dialog — so this card
// changes an account that is already there rather than making one.
function AccessCard() {
  const [auth, setAuth] = useState<AuthStatus | null>(null);
  const [user, setUser] = useState('');
  const [current, setCurrent] = useState('');
  const [next, setNext] = useState('');
  const [repeat, setRepeat] = useState('');
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);

  const load = () =>
    apiJSON<AuthStatus>('/api/auth')
      .then((r) => {
        if (!r.ok) return;
        setAuth(r.data);
        setUser(r.data.user || '');
      })
      .catch(() => {});
  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  if (!auth) return null;
  const min = auth.min_length || 8;

  async function save() {
    const name = user.trim();
    if (!USER_NAME_RULE.test(name)) return setError(USER_NAME_REFUSAL);
    if (next !== '') {
      if (next.length < min) return setError('A password is at least ' + min + ' characters.');
      if (next !== repeat) return setError('The two password entries do not match.');
    }
    setError('');
    setSaving(true);
    const body: Record<string, string> = { user: name, current };
    if (next !== '') body.password = next;
    const res = await apiJSON<{ error?: string }>('/api/auth', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    }).catch(() => ({ ok: false, data: { error: 'No answer.' } }));
    setSaving(false);
    if (!res.ok) return setError(res.data.error || 'The credentials could not be changed.');
    setCurrent('');
    setNext('');
    setRepeat('');
    toast('auth', 'The account is now ' + name);
    load();
  }

  return html`<div class="card" style="max-width:720px">
    <div class="card-head"><h3>Access</h3>
      <span class="chip ok mono">${auth.user}</span>
    </div>
    <div class="card-body">
      <p class="muted" style="margin:0 0 14px">There is one account. Its name and password open
        this interface, the SMB, FTP and SFTP shares of the image library, and an${' '}
        <span class="mono">ssh</span> login. Changing them here changes them everywhere.</p>
      <div class="set-grid">
        <div class="set-name">User name</div>
        <div class="set-val">
          <input type="text" autocapitalize="off" spellcheck="false" autocomplete="username"
            value=${user} disabled=${saving}
            onInput=${(e: Event) => setUser((e.target as HTMLInputElement).value)} />
        </div>
        <div class="set-info">Renames the account. It does not add a second one. ${USER_NAME_HELP}</div>

        <div class="set-name">Current password</div>
        <div class="set-val">
          <input type="password" autocomplete="current-password" value=${current}
            disabled=${saving}
            onInput=${(e: Event) => setCurrent((e.target as HTMLInputElement).value)} />
        </div>
        <div class="set-info">The password you are logged in with. Required for any change here,
          including a change of name.</div>

        <div class="set-name">New password</div>
        <div class="set-val">
          <input type="password" autocomplete="new-password" value=${next} disabled=${saving}
            onInput=${(e: Event) => setNext((e.target as HTMLInputElement).value)} />
        </div>
        <div class="set-info">Leave empty to change the name only.</div>

        ${next !== ''
          ? html`<div class="set-name">Repeat</div>
            <div class="set-val">
              <input type="password" autocomplete="new-password" value=${repeat}
                disabled=${saving}
                onInput=${(e: Event) => setRepeat((e.target as HTMLInputElement).value)} />
            </div>
            <div class="set-info">At least ${min} characters.</div>`
          : null}
      </div>
      ${error ? html`<p class="upd-warn">${error}</p>` : null}
      <p class="muted" style="margin:12px 0 0">This browser stays signed in - the change hands
        it a new session. Every other browser, and anything signed in elsewhere, is signed
        out at once.</p>
      <div style="display:flex; gap:8px; justify-content:flex-end; margin-top:12px">
        <button class="btn primary" disabled=${saving || current === ''}
          onClick=${save}>Save</button>
      </div>
    </div></div>`;
}

// The host name and the ssh key that reaches a shell — the two the first-run
// dialog asks for beside the credentials, offered again here for an installation
// that is already set up. The key goes on the operator's account.
function NetworkCard() {
  const [hostname, setHostname] = useState<string | null>(null);
  const [name, setName] = useState('');
  const [key, setKey] = useState('');
  const [keyUser, setKeyUser] = useState('');
  const [hasKey, setHasKey] = useState(false);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const load = () => {
    apiJSON<{ hostname: string }>('/api/hostname')
      .then((r) => {
        if (!r.ok) return;
        setHostname(r.data.hostname || '');
        setName(r.data.hostname || '');
      })
      .catch(() => {});
    apiJSON<{ user: string; configured: boolean }>('/api/sshkey')
      .then((r) => {
        if (!r.ok) return;
        setKeyUser(r.data.user || '');
        setHasKey(!!r.data.configured);
      })
      .catch(() => {});
  };
  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  if (hostname === null) return null;

  async function rename() {
    const wanted = name.trim();
    if (!HOST_NAME_RULE.test(wanted)) return setError(HOST_NAME_REFUSAL);
    setError('');
    setBusy(true);
    const res = await apiJSON<{ error?: string; hostname?: string }>('/api/hostname', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ hostname: wanted }),
    }).catch(() => ({ ok: false, data: { error: 'No answer.' } }));
    setBusy(false);
    if (!res.ok) return setError(res.data.error || 'The host name could not be changed.');
    toast('hostname', 'The host name is now ' + wanted);
    load();
  }

  async function saveKey() {
    const k = key.trim();
    if (!SSH_KEY_RULE.test(k)) return setError(SSH_KEY_REFUSAL);
    setError('');
    setBusy(true);
    const res = await apiJSON<{ error?: string }>('/api/sshkey', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ key: k }),
    }).catch(() => ({ ok: false, data: { error: 'No answer.' } }));
    setBusy(false);
    if (!res.ok) return setError(res.data.error || 'The key could not be installed.');
    setKey('');
    toast('sshkey', 'The key is installed');
    load();
  }

  return html`<div class="card" style="max-width:720px; margin-bottom:14px">
    <div class="card-head"><h3>Network and shell</h3>
      <span class="chip ok mono">${hostname || '—'}</span>
    </div>
    <div class="card-body">
      <div class="set-grid">
        <div class="set-name">Host name</div>
        <div class="set-val" style="display:flex; gap:8px">
          <input type="text" autocapitalize="off" spellcheck="false" value=${name}
            disabled=${busy}
            onInput=${(e: Event) => setName((e.target as HTMLInputElement).value)} />
          <button class="btn small" disabled=${busy || name.trim() === hostname}
            onClick=${rename}>Rename</button>
        </div>
        <div class="set-info">The name this QUniLator has on the network. Renaming it moves${' '}
          <span class="mono">${(name.trim() || hostname) + '.local'}</span>, the DNS-SD entry, the
          DHCP lease and the login banner together. ${HOST_NAME_REFUSAL}</div>

        <div class="set-name">SSH key</div>
        <div class="set-val">
          <textarea class="mono" rows="3" autocapitalize="off" spellcheck="false"
            placeholder="ssh-ed25519 AAAA… you@workstation" value=${key} disabled=${busy || !keyUser}
            onInput=${(e: Event) => setKey((e.target as HTMLTextAreaElement).value)}></textarea>
        </div>
        <div class="set-info">${
          keyUser
            ? html`Paste a public key and${' '}
                <span class="mono">${keyUser}</span> reaches a shell over ssh, with sudo once
                there. ${hasKey
                  ? 'A key is installed; a new one replaces it.'
                  : 'No key is installed, so the account has no shell login yet.'}`
            : html`The key goes on the operator's account, and there is none yet.`
        }</div>
      </div>
      ${error ? html`<p class="upd-warn">${error}</p>` : null}
      <div style="display:flex; gap:8px; justify-content:flex-end; margin-top:12px">
        <button class="btn primary" disabled=${busy || key.trim() === ''}
          onClick=${saveKey}>Install key</button>
      </div>
    </div></div>`;
}

interface SerialPort {
  device: string;
  where: string;
  login: boolean;
  kernel_console: boolean;
  used_by: string;
}
interface SerialPorts {
  ports: SerialPort[];
  reboot_required: boolean;
  warnings?: string[];
  error?: string;
}

// The BeagleBone's three UARTs, each either a Linux login or the emulator's. One
// login is kept at all times, so a card whose network has gone is still
// reachable with a terminal. The kernel console follows the first port that
// carries a login, which is a boot setting and says so.
function SerialPortsCard() {
  const [state, setState] = useState<SerialPorts | null>(null);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const load = () => {
    apiJSON<SerialPorts>('/api/serialports')
      .then((r) => setState(r.ok ? r.data : { ports: [], reboot_required: false }))
      .catch(() => setState({ ports: [], reboot_required: false }));
  };
  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  if (state === null || !state.ports.length) return null;

  async function toggle(device: string, login: boolean) {
    const s = state as SerialPorts;
    const logins = s.ports
      .filter((p) => (p.device === device ? login : p.login))
      .map((p) => p.device);
    setError('');
    setBusy(true);
    const res = await apiJSON<SerialPorts>('/api/serialports', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ logins }),
    }).catch(() => ({ ok: false, data: { error: 'QUniLator did not answer.' } as SerialPorts }));
    setBusy(false);
    if (!res.ok) {
      load();
      return setError(res.data.error || 'The ports could not be set.');
    }
    setState(res.data);
    const warns = (res.data.warnings || []).join('; ');
    if (warns) setError(warns);
    toast(
      'serialports',
      login ? 'A login answers on ' + device : device + ' is free for the emulator',
    );
  }

  const logins = state.ports.filter((p) => p.login);
  return html`<div class="card" style="max-width:720px; margin-bottom:14px">
    <div class="card-head"><h3>Serial ports</h3>
      <span class="chip ok mono">${logins.map((p) => p.device).join(' ') || '—'}</span>
    </div>
    <div class="card-body">
      <div class="set-grid">
        ${state.ports.map(
          (p) => html`
          <div class="set-name mono">${p.device}</div>
          <div class="set-val" style="display:flex; gap:8px; align-items:center">
            <label><input type="checkbox" checked=${p.login}
              disabled=${busy || (p.login && logins.length === 1)}
              onChange=${() => toggle(p.device, !p.login)} /> Linux login</label>
            ${p.kernel_console ? html`<span class="chip">kernel console</span>` : null}
            ${p.used_by ? html`<span class="chip warn mono">${p.used_by}</span>` : null}
          </div>
          <div class="set-info">${p.where}. ${
            p.login
              ? 'A terminal here reaches a shell on the BeagleBone.'
              : p.used_by
                ? html`Held by <span class="mono">${p.used_by}</span>.`
                : 'Free — an emulated DL11 or the console bridge can take it.'
          }</div>`,
        )}
      </div>
      <p class="muted" style="margin-top:12px">One port keeps its login: it is the way back onto a
        card whose network has gone. The USB port carries a login of its own as well. A port the
        emulator holds cannot take one — disable the device first.</p>
      ${
        state.reboot_required
          ? html`<p class="upd-warn">The kernel console moves with the login. Reboot the
              BeagleBone to stop it printing on the port it was on.</p>`
          : null
      }
      ${error ? html`<p class="upd-warn">${error}</p>` : null}
    </div></div>`;
}

// What this QUniLator has recorded. A recording is downloaded and read with
// `qcon render`, which turns it into a page with the typing marked off from
// what the machine printed; the file itself is standard asciicast, so an
// asciinema player takes it too.
function RecordingsCard() {
  const [items, setItems] = useState<Recording[] | null>(null);
  const [tick, setTick] = useState(0);
  useEffect(() => {
    listRecordings()
      .then(setItems)
      .catch(() => setItems([]));
  }, [tick]);
  if (items === null)
    return html`<div class="card" style="max-width:720px">
      <div class="card-head"><h3>Console recordings</h3></div>
      <div class="card-body"><p class="muted">Reading…</p></div></div>`;
  return html`<div class="card" style="max-width:720px">
    <div class="card-head"><h3>Console recordings</h3>
      <button class="btn small" onClick=${() => setTick(tick + 1)}>Refresh</button>
    </div>
    <div class="card-body">
      ${
        items.length === 0
          ? html`<p class="muted">None yet. Record a session with the Record button
              on the dashboard's console card; both what the machine printed and
              what was typed at it are captured, whoever typed it.</p>`
          : html`<table class="rec-table"><tbody>
              ${items.map(
                (r) => html`<tr>
                  <td class="mono">${r.name}</td>
                  <td class="muted">${r.mtime}</td>
                  <td class="muted">${(r.bytes / 1024).toFixed(0)} KB</td>
                  <td>
                    <a class="btn small" href=${'/api/recordings/' + encodeURIComponent(r.name)}
                       download=${r.name}>Download</a>
                    <button class="btn small danger" onClick=${async () => {
                      await deleteRecording(r.name);
                      setTick(tick + 1);
                    }}>Delete</button>
                  </td>
                </tr>`,
              )}
            </tbody></table>`
      }
    </div>
  </div>`;
}

// One page per function, all under /system. The sidebar opens the sub-menu when
// System is the active entry, so a page here carries no navigation of its own.

export function SystemAccessPage() {
  return html`<section class="page active" data-page="system-access">
    <p class="lede">The one account that reaches this QUniLator — the web interface, the file
      shares and ssh alike.</p>
    <${AccessCard} />
  </section>`;
}

export function SystemNetworkPage() {
  return html`<section class="page active" data-page="system-network">
    <p class="lede">What this QUniLator is called on the network, and the key that reaches a
      shell on it.</p>
    <${NetworkCard} />
  </section>`;
}

export function SystemSerialPage() {
  return html`<section class="page active" data-page="system-serial">
    <p class="lede">Which of the BeagleBone's UARTs carry a Linux login, and which are free for
      the emulator.</p>
    <${SerialPortsCard} />
  </section>`;
}

export function SystemRecordingsPage() {
  return html`<section class="page active" data-page="system-recordings">
    <p class="lede">Console sessions captured with Record on the dashboard.</p>
    <${RecordingsCard} />
  </section>`;
}

// The update page carries the two things a card can update, in the order they
// matter: the emulator package this QUniLator is, and the Debian underneath it.
export function SystemUpdatePage() {
  const s = useStore();
  const [tick, setTick] = useState(0);
  useEffect(() => {
    refreshUpdate().catch(() => {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick]);
  const u = s.update;
  if (!u)
    return html`<section class="page active" data-page="system-update">
      <p class="lede">What this QUniLator runs, and what the repository offers.</p>
      <p class="muted">Reading the update status…</p></section>`;

  const busy = updateRunning(u);
  const checking = u.state === 'checking';
  const last = u.last || {};
  return html`<section class="page active" data-page="system-update">
    <p class="lede">What this QUniLator runs, and what the repository offers.</p>

    <div class="card" style="max-width:720px">
      <div class="card-head"><h3>${u.package || 'emulator'}</h3><${StateChip} u=${u} /></div>
      <div class="card-body">
        <div class="set-grid">
          <div class="set-name">Installed</div>
          <div class="set-val"><span class="pill mono">${u.installed || s.serverVersion || '—'}</span>
            ${s.serverBuilt ? html`<span class="muted">built ${when(s.serverBuilt)}</span>` : null}</div>
          <div class="set-info">The version of the ${u.package} package this QUniLator runs. This page was
            served by ${bundleVersion === s.serverVersion ? 'it' : html`version <span class="mono">${bundleVersion}</span>`}.</div>

          <div class="set-name">Available</div>
          <div class="set-val">
            ${u.candidate ? html`<span class="pill mono">${u.candidate}</span>` : html`<span class="muted">—</span>`}
            <button class="btn small" disabled=${busy || checking} onClick=${async () => {
              await checkForUpdate();
              setTick(tick + 1);
            }}>${checking ? 'Checking…' : 'Check now'}</button>
          </div>
          <div class="set-info">${checking
            ? html`<strong>Checking for new updates</strong> — this takes a minute or two. `
            : null}Last checked ${when(u.checked_at)}.</div>

          <${UpdateFileRow} u=${u} busy=${busy} />
        </div>

        ${
          !u.source_configured
            ? html`<p class="upd-warn">This QUniLator has no update source: the package was
                installed by hand, so there is no apt repository to check. Installing it once from
                the repository makes it updateable.</p>`
            : null
        }
        ${
          u.needs_repair
            ? html`<p class="upd-warn">The package system needs repair — a dpkg was interrupted.
                The next install repairs it first; <span class="mono">dpkg --configure -a</span>
                does it now.</p>`
            : null
        }
        ${
          u.state === 'ahead'
            ? html`<p class="muted">The repository offers <span class="mono">${u.candidate}</span>,
                older than the <span class="mono">${u.installed}</span> this QUniLator runs — a
                hand-built package. Nothing is offered.</p>`
            : null
        }
        ${
          u.error && !busy
            ? html`<p class="upd-warn">${u.error}</p>`
            : null
        }
        ${
          busy
            ? u.state === 'os-upgrading'
              ? html`<p class="muted">The operating system is being upgraded; its progress is below.</p>`
              : html`<p class="muted">An update is running: <span class="mono">${u.state}</span>.</p>`
            : u.local?.version
              ? html`<${InstallDialog} u=${u} version=${u.local.version} local=${true}
                  onDone=${() => setTick(tick + 1)} />`
            : updateAvailable(u)
              ? html`<${InstallDialog} u=${u} version=${u.candidate} onDone=${() => setTick(tick + 1)} />`
              : u.candidate && u.candidate === u.dismissed && u.candidate !== u.installed
                ? html`<p class="muted"><span class="mono">${u.candidate}</span> is available and
                    dismissed. <button class="btn small" onClick=${async () => {
                      await dismissUpdate('');
                      setTick(tick + 1);
                    }}>Announce it again</button></p>`
                : null
        }

        ${
          last.state
            ? html`<div class="upd-last">
                <div class="set-name">Last update</div>
                <p class="muted">${last.state === 'done'
                  ? 'Installed'
                  : last.state === 'rolled-back'
                    ? 'Rolled back'
                    : 'Failed'} — <span class="mono">${last.from || '—'}</span> → ${' '}<span class="mono">${last.to || '—'}</span>, ${when(last.at || '')}.</p>
                ${
                  u.journal && u.journal.length && u.state !== 'os-upgrading'
                    ? html`<pre class="upd-changelog">${u.journal.join('\n')}</pre>`
                    : null
                }
                ${
                  u.rollback
                    ? html`<p class="muted">A cached package is kept, so ${' '}<span class="mono">qunilator-update --rollback</span> over ssh steps this QUniLator back a version.</p>`
                    : null
                }
              </div>`
            : null
        }
      </div></div>

    <${OsRow} u=${u} />
  </section>`;
}
