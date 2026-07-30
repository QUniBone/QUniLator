// The System page: what this board runs, and updating it.
//
// Two rows, kept apart because they promise different things. The emulator update
// is reversible - the board watches the new version come up and puts the old one
// back if it does not - and it stops the emulated machine, so it says so and asks
// for a confirmation. The operating-system upgrade cannot be undone by apt at
// all, and leaves the machine running, so it says that instead.
import { html } from '../html';
import { useEffect, useState } from 'preact/hooks';
import { useStore } from '../store';
import { liveControl } from '../api';
import { confirmModal } from '../lib/modals';
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
      <p class="lede">What this board runs, and updating it.</p>
      <p class="muted">Reading the update status…</p></section>`;

  const busy = updateRunning(u);
  const last = u.last || {};
  return html`<section class="page active" data-page="system">
    <p class="lede">What this board runs, and updating it.</p>

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
