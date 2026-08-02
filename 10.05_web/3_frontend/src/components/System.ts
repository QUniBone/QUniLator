// The System page: who reaches this board, what it runs, and updating it.
//
// Access comes first: the one pair of credentials that reaches the board, by the
// browser and by the file shares alike.
//
// The two update rows are kept apart because they promise different things. The
// emulator update is reversible - the board watches the new version come up and
// puts the old one back if it does not - and it stops the emulated machine, so it
// says so and asks for a confirmation. The operating-system upgrade cannot be
// undone by apt at all, and leaves the machine running, so it says that instead.
import { html } from '../html';
import { useEffect, useState } from 'preact/hooks';
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
  dismissUpdate,
  installUpdate,
  upgradeOs,
  updateAvailable,
  updateRunning,
} from '../lib/update';
import type { UpdateStatus } from '../types';

// "2026-07-30T09:12:00Z" as something a person reads, in local time
function when(iso: string): string {
  if (!iso) return 'never';
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  return d.toLocaleString();
}

function StateChip({ u }: { u: UpdateStatus }) {
  if (updateRunning(u)) return html`<span class="chip warn">${u.state}</span>`;
  if (u.state === 'failed') return html`<span class="chip err">failed</span>`;
  if (u.state === 'rolled-back') return html`<span class="chip err">rolled back</span>`;
  if (u.state === 'ahead')
    return html`<span class="chip out" title="the repository offers an older version than this board runs">ahead of the repository</span>`;
  if (updateAvailable(u)) return html`<span class="chip warn">update available</span>`;
  if (!u.source_configured) return html`<span class="chip off">no update source</span>`;
  return html`<span class="chip ok">up to date</span>`;
}

// The changelog since the installed version, fetched when the operator asks for
// it: reading it makes the board download the candidate, which is also the
// staging step an install wants, so it is not fetched on every page view.
function Changelog({ open }: { open: boolean }) {
  const [text, setText] = useState<string | null>(null);
  useEffect(() => {
    if (!open || text !== null) return;
    setText('');
    fetchChangelog().then((t) => setText(t || 'The changelog could not be read.'));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);
  if (!open) return null;
  if (text === '') return html`<p class="muted">Fetching the changelog…</p>`;
  return html`<pre class="upd-changelog">${text}</pre>`;
}

// What an install costs, said before the button rather than after: the machine
// stops, its configuration is the startup one again, and unsaved edits are lost.
function InstallDialog({ u, onDone }: { u: UpdateStatus; onDone: () => void }) {
  const s = useStore();
  const [confirmed, setConfirmed] = useState(false);
  const [showLog, setShowLog] = useState(true);
  const running = !!s.hw.powered && !s.bus.halted;
  const modified = s.configModified === true;
  return html`<div class="upd-install">
    <${Changelog} open=${showLog} />
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
      <li>The board comes back up in its <strong>startup configuration</strong>${
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
        onClick=${() => installUpdate(u.candidate)}>Install ${u.candidate}</button>
      <button class="btn" onClick=${async () => {
        await dismissUpdate(u.candidate);
        onDone();
      }}>Dismiss</button>
    </div>
  </div>`;
}

function OsRow({ u }: { u: UpdateStatus }) {
  const os = u.os || { count: 0, packages: [], held_back: [], reboot_required: false };
  const [open, setOpen] = useState(false);
  const busy = updateRunning(u);
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
          : html`<p class="muted" style="margin:0 0 10px">Nothing else on the board has an update.</p>`
      }
      ${
        os.held_back && os.held_back.length
          ? html`<p class="muted" style="margin:0 0 10px">Held back: <span class="mono">${os.held_back.join(', ')}</span>.
            The kernel is held deliberately — the cape port depends on it.</p>`
          : null
      }
      ${
        os.reboot_required
          ? html`<p class="upd-warn">A reboot is required to finish an earlier upgrade.
            Nothing here reboots the board; a reboot stops the emulated machine, so it is your call when.</p>`
          : null
      }
      ${
        os.count
          ? html`<p class="upd-warn"><strong>This cannot be undone.</strong> apt cannot reverse an
              upgrade, so an operating-system upgrade that breaks the board is an ssh session or a
              reflash — unlike the emulator update, which puts the old version back by itself. The
              emulator package is held back, so the emulated machine keeps running throughout.</p>
            <button class="btn" disabled=${busy} onClick=${async () => {
              const ok = await confirmModal(
                'Upgrade the operating system',
                'This upgrades ' +
                  os.count +
                  " of the board's packages. It cannot be undone: apt has no way to " +
                  'reverse an upgrade. The emulated machine keeps running, and the board is not ' +
                  'rebooted.',
                'Upgrade'
              );
              if (ok) await upgradeOs();
            }}>Upgrade the operating system</button>`
          : null
      }
    </div></div>`;
}

type AuthStatus = {
  configured: boolean;
  source: string;
  user: string;
  min_length: number;
};

// The credentials this board answers to. The user name is an account on the
// board as well, so the same pair reaches the image library over SMB, FTP and
// SFTP; a board that carries only a password takes whatever name the browser
// offers, and says so until one is set.
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
  const fromEnvironment = auth.source === 'environment';

  async function save() {
    const name = user.trim();
    if (name !== '' && !USER_NAME_RULE.test(name)) return setError(USER_NAME_REFUSAL);
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
    }).catch(() => ({ ok: false, data: { error: 'The board did not answer.' } }));
    setSaving(false);
    if (!res.ok) return setError(res.data.error || 'The credentials could not be changed.');
    setCurrent('');
    setNext('');
    setRepeat('');
    toast('auth', name ? 'The board answers to ' + name : 'The board takes any user name');
    load();
  }

  return html`<div class="card" style="max-width:720px">
    <div class="card-head"><h3>Access</h3>
      ${auth.user
        ? html`<span class="chip ok mono">${auth.user}</span>`
        : html`<span class="chip warn">any user name</span>`}
    </div>
    <div class="card-body">
      ${fromEnvironment
        ? html`<p class="muted">The credentials come from <span class="mono">WEBUI_PASSWORD</span>
            in the service environment, so they are set outside this interface and any user name is
            accepted. Remove that setting and restart the service to set a name here.</p>`
        : html`
        <div class="set-grid">
          <div class="set-name">User name</div>
          <div class="set-val">
            <input type="text" autocapitalize="off" spellcheck="false" autocomplete="username"
              value=${user} disabled=${saving}
              onInput=${(e: Event) => setUser((e.target as HTMLInputElement).value)} />
          </div>
          <div class="set-info">${
            auth.user
              ? html`The browser prompt takes this name, and so do the SMB, FTP and SFTP shares of
                  the image library. Changing it moves the board's file-share account to the new
                  name.`
              : html`No user name is set, so the browser prompt takes any name and the file shares
                  answer to <span class="mono">qunilator</span>. Setting one here makes a single pair
                  of credentials reach the board by every route.`
          } ${USER_NAME_HELP}</div>

          <div class="set-name">Current password</div>
          <div class="set-val">
            <input type="password" autocomplete="current-password" value=${current}
              disabled=${saving}
              onInput=${(e: Event) => setCurrent((e.target as HTMLInputElement).value)} />
          </div>
          <div class="set-info">Changing either half takes the password in force.</div>

          <div class="set-name">New password</div>
          <div class="set-val">
            <input type="password" autocomplete="new-password" value=${next} disabled=${saving}
              onInput=${(e: Event) => setNext((e.target as HTMLInputElement).value)} />
          </div>
          <div class="set-info">Leave empty to keep the password and change the name alone.</div>

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
        <p class="muted" style="margin:12px 0 0">The browser holds the old credentials until it is
          asked again, so the next request after a change brings up its prompt.</p>
        <div style="display:flex; gap:8px; justify-content:flex-end; margin-top:12px">
          <button class="btn primary" disabled=${saving || current === ''}
            onClick=${save}>Save</button>
        </div>`}
    </div></div>`;
}

// The board's own name and the ssh key that reaches its shell — the two the
// first-run dialog asks for beside the credentials, offered again here for a
// board that is already set up. The key goes on the operator's account, so a
// board with no user name set has nowhere to put one and says so.
function BoardCard() {
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
    }).catch(() => ({ ok: false, data: { error: 'The board did not answer.' } }));
    setBusy(false);
    if (!res.ok) return setError(res.data.error || 'The board could not be renamed.');
    toast('hostname', 'The board is now named ' + wanted);
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
    }).catch(() => ({ ok: false, data: { error: 'The board did not answer.' } }));
    setBusy(false);
    if (!res.ok) return setError(res.data.error || 'The key could not be installed.');
    setKey('');
    toast('sshkey', 'The board answers that key');
    load();
  }

  return html`<div class="card" style="max-width:720px; margin-bottom:14px">
    <div class="card-head"><h3>Board</h3>
      <span class="chip ok mono">${hostname || '—'}</span>
    </div>
    <div class="card-body">
      <div class="set-grid">
        <div class="set-name">Name</div>
        <div class="set-val" style="display:flex; gap:8px">
          <input type="text" autocapitalize="off" spellcheck="false" value=${name}
            disabled=${busy}
            onInput=${(e: Event) => setName((e.target as HTMLInputElement).value)} />
          <button class="btn small" disabled=${busy || name.trim() === hostname}
            onClick=${rename}>Rename</button>
        </div>
        <div class="set-info">What the board answers to on the network —
          <span class="mono">${(name.trim() || hostname) + '.local'}</span>, the DNS-SD entry, the
          DHCP lease and the login banner all follow it. ${HOST_NAME_REFUSAL}</div>

        <div class="set-name">SSH key</div>
        <div class="set-val">
          <textarea class="mono" rows="3" autocapitalize="off" spellcheck="false"
            placeholder="ssh-ed25519 AAAA… you@workstation" value=${key} disabled=${busy || !keyUser}
            onInput=${(e: Event) => setKey((e.target as HTMLTextAreaElement).value)}></textarea>
        </div>
        <div class="set-info">${
          keyUser
            ? html`A public key pasted here is what
                <span class="mono">${keyUser}</span> reaches a shell on this board with, and sudo
                once there. ${hasKey
                  ? 'A key is installed; a new one replaces it.'
                  : 'No key is installed, so the account has no shell login yet.'}`
            : html`Set a user name under Access first — the key goes on that account.`
        }</div>
      </div>
      ${error ? html`<p class="upd-warn">${error}</p>` : null}
      <div style="display:flex; gap:8px; justify-content:flex-end; margin-top:12px">
        <button class="btn primary" disabled=${busy || key.trim() === ''}
          onClick=${saveKey}>Install key</button>
      </div>
    </div></div>`;
}

// What the board has recorded. A recording is downloaded and read with
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

export function SystemPage() {
  const s = useStore();
  const [tick, setTick] = useState(0);
  useEffect(() => {
    refreshUpdate().catch(() => {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick]);
  const u = s.update;
  if (!u)
    return html`<section class="page active" data-page="system">
      <p class="lede">Who reaches this board, what it runs, and updating it.</p>
      <${AccessCard} />
      <${BoardCard} />
      <p class="muted">Reading the update status…</p></section>`;

  const busy = updateRunning(u);
  const last = u.last || {};
  return html`<section class="page active" data-page="system">
    <p class="lede">Who reaches this board, what it runs, and updating it.</p>

    <${AccessCard} />

    <${BoardCard} />

    <${RecordingsCard} />

    <div class="card" style="max-width:720px">
      <div class="card-head"><h3>${u.package || 'emulator'}</h3><${StateChip} u=${u} /></div>
      <div class="card-body">
        <div class="set-grid">
          <div class="set-name">Installed</div>
          <div class="set-val"><span class="pill mono">${u.installed || s.serverVersion || '—'}</span>
            ${s.serverBuilt ? html`<span class="muted">built ${when(s.serverBuilt)}</span>` : null}</div>
          <div class="set-info">The version of the ${u.package} package this board runs. This page was
            served by ${bundleVersion === s.serverVersion ? 'it' : html`version <span class="mono">${bundleVersion}</span>`}.</div>

          <div class="set-name">Available</div>
          <div class="set-val">
            ${u.candidate ? html`<span class="pill mono">${u.candidate}</span>` : html`<span class="muted">—</span>`}
            <button class="btn small" disabled=${busy} onClick=${async () => {
              await checkForUpdate();
              setTick(tick + 1);
            }}>Check now</button>
          </div>
          <div class="set-info">Last checked ${when(u.checked_at)}.</div>
        </div>

        ${
          !u.source_configured
            ? html`<p class="upd-warn">This board has no update source: the package was installed by
                hand, so there is no apt repository to check. Installing it once from the repository
                makes the board updateable.</p>`
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
                older than the <span class="mono">${u.installed}</span> this board runs — a
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
            ? html`<p class="muted">An update is running: <span class="mono">${u.state}</span>.</p>`
            : updateAvailable(u)
              ? html`<${InstallDialog} u=${u} onDone=${() => setTick(tick + 1)} />`
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
                  u.journal && u.journal.length
                    ? html`<pre class="upd-changelog">${u.journal.join('\n')}</pre>`
                    : null
                }
                ${
                  u.rollback
                    ? html`<p class="muted">A cached package is kept, so ${' '}<span class="mono">qunilator-update --rollback</span> over ssh steps this board back a version.</p>`
                    : null
                }
              </div>`
            : null
        }
      </div></div>

    <${OsRow} u=${u} />
  </section>`;
}
