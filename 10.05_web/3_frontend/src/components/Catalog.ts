// The Catalogue screen: what the subscribed catalogues offer, and the board's
// own fetch-and-import of a chosen machine. The board does the downloading —
// this page only starts the job and watches the "catalog" event frames.
import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import { useStore } from '../store';
import {
  loadCatalog,
  loadConfigs,
  catalogRefresh,
  catalogFetch,
  catalogCancel,
  putCatalogSources,
} from '../api';
import { humanSize } from '../lib/util';
import { promptModal, alertModal } from '../lib/modals';
import { Chip } from './common';
import type { CatalogEntry, CatalogJob, CatalogSource } from '../types';

const BUS_WORD: Record<string, string> = { qbus: 'QBus', unibus: 'Unibus', any: 'any bus' };

// A job the page should be showing: a fetch under way, or its outcome.
function jobVisible(job: CatalogJob | null): boolean {
  return !!job && job.mode === 'fetch' && job.state !== 'idle';
}

function jobPhase(job: CatalogJob): string {
  switch (job.state) {
    case 'starting':
      return 'Starting…';
    case 'downloading':
      return job.bytes_total > 0
        ? 'Downloading — ' + humanSize(job.bytes_done) + ' of ' + humanSize(job.bytes_total)
        : 'Downloading — ' + humanSize(job.bytes_done);
    case 'verifying':
      return 'Verifying the checksum…';
    case 'extracting':
      return 'Unpacking ' + (job.file || 'images') + ' (' + (job.files_done + 1)
        + ' of ' + job.files_total + ')';
    case 'importing':
      return 'Writing the configuration…';
    case 'done':
      return 'Imported as “' + job.config + '”.';
    case 'failed':
      return 'The import failed.';
    case 'cancelled':
      return 'Cancelled.';
    default:
      return '';
  }
}

function JobCard({ job, onDismiss }: { job: CatalogJob; onDismiss: () => void }) {
  const running = ['starting', 'downloading', 'verifying', 'extracting', 'importing'].includes(
    job.state
  );
  const frac = job.bytes_total > 0 ? Math.min(1, job.bytes_done / job.bytes_total) : 0;
  const barVisible = job.state === 'downloading' || job.state === 'extracting';
  return html`<div class="card cat-job">
    <div class="card-head">
      <h3>${job.title || job.entry}</h3>
      ${running
        ? html`<button class="btn small" onClick=${() => catalogCancel()}>Cancel</button>`
        : html`<button class="btn small" onClick=${onDismiss}>Dismiss</button>`}
    </div>
    <div class="card-body">
      <div class=${'cat-phase' + (job.state === 'failed' ? ' err' : '')}>${jobPhase(job)}</div>
      ${barVisible &&
      html`<div class="upd-bar cat-bar">
        <div class="upd-bar-fill" style=${'width:' + (frac * 100).toFixed(1) + '%'}></div>
      </div>`}
      ${job.state === 'failed' && job.error && html`<div class="cat-note err">${job.error}</div>`}
      ${job.state === 'done' &&
      html`<div class="cat-note">
        ${job.images_written} image${job.images_written === 1 ? '' : 's'} written${job
          .images_kept.length > 0
          ? ', ' + job.images_kept.length + ' already here and kept'
          : ''}.
        ${job.note ? ' ' + job.note + '.' : ''} ${job.autostart_note ? ' ' + job.autostart_note + '.' : ''}
      </div>`}
    </div>
  </div>`;
}

function EntryRow({
  source,
  entry,
  busy,
}: {
  source: CatalogSource;
  entry: CatalogEntry;
  busy: boolean;
}) {
  const doImport = async () => {
    const name = await promptModal(
      'Import from the catalogue',
      'Name on this QUniLator',
      entry.id,
      'Fetch and import'
    );
    if (name === null) return;
    const r = await catalogFetch(source.url, entry.id, name.trim());
    if (!r.ok) await alertModal('Import refused', r.error || 'QUniLator refused it');
  };
  const disabled = busy || entry.imported || !entry.bus_ok;
  return html`<div class="cat-entry">
    <div class="cat-entry-main">
      <div class="cat-entry-top">
        ${entry.page
          ? html`<a class="cat-entry-name" href=${entry.page} target="_blank" rel="noopener"
              >${entry.title || entry.id}</a>`
          : html`<span class="cat-entry-name">${entry.title || entry.id}</span>`}
        ${entry.guest && html`<span class="pill mono">${entry.guest}</span>`}
        ${entry.imported && html`<${Chip} cls="ok">imported</${Chip}>`}
        ${!entry.bus_ok &&
        html`<${Chip} cls="warn">${BUS_WORD[entry.bus || ''] || entry.bus} only</${Chip}>`}
      </div>
      ${entry.summary && html`<div class="cat-entry-desc">${entry.summary}</div>`}
      <div class="cat-entry-sub muted">
        ${entry.download?.bytes ? humanSize(entry.download.bytes) + ' download' : ''}
        ${entry.images_total > 0
          ? ' · ' +
            (entry.images_present === entry.images_total
              ? 'all ' + entry.images_total + ' images already here'
              : entry.images_present > 0
                ? entry.images_present + ' of ' + entry.images_total + ' images already here'
                : entry.images_total + ' image' + (entry.images_total === 1 ? '' : 's'))
          : ''}
      </div>
    </div>
    <button class="btn small primary" disabled=${disabled} onClick=${doImport}>Import</button>
  </div>`;
}

function SourceCard({ source, busy }: { source: CatalogSource; busy: boolean }) {
  const idx = source.index;
  const entries = idx?.configurations || [];
  return html`<div class="card cat-source">
    <div class="card-head">
      <h3>${idx?.name || source.url}</h3>
      ${!source.ok && source.error && html`<${Chip} cls="warn">unreachable</${Chip}>`}
    </div>
    <div class="card-body">
      ${idx?.name && html`<div class="cat-source-sub muted mono">${source.url}</div>`}
      ${!source.ok &&
      source.error &&
      html`<div class="cat-note err">
        ${source.error}${source.fetched_at ? ' — showing what it offered ' + source.fetched_at : ''}
      </div>`}

      ${entries.length > 0
        ? entries.map(
            (e) => html`<${EntryRow} key=${e.id} source=${source} entry=${e} busy=${busy} />`
          )
        : source.ok || source.index
          ? html`<div class="muted">This catalogue offers nothing yet.</div>`
          : source.error
            ? null
            : html`<div class="muted">Not fetched yet — refresh to ask it.</div>`}
    </div>
  </div>`;
}

function SourcesEditor({ sources }: { sources: string[] }) {
  const [draft, setDraft] = useState('');
  const save = (next: string[]) => putCatalogSources(next);
  const add = () => {
    const url = draft.trim();
    if (!url) return;
    setDraft('');
    save([...sources, url]);
  };
  return html`<div class="card cat-editor">
    <div class="card-head"><h3>Subscribed catalogues</h3></div>
    <div class="card-body">
      ${sources.map(
        (url, i) => html`<div class="cat-url-row" key=${url}>
          <span class="mono cat-url">${url}</span>
          <button class="btn small" title="Move up" disabled=${i === 0}
            onClick=${() => {
              const next = sources.slice();
              [next[i - 1], next[i]] = [next[i], next[i - 1]];
              save(next);
            }}>↑</button>
          <button class="btn small" title="Move down" disabled=${i === sources.length - 1}
            onClick=${() => {
              const next = sources.slice();
              [next[i], next[i + 1]] = [next[i + 1], next[i]];
              save(next);
            }}>↓</button>
          <button class="btn small danger" title="Remove"
            onClick=${() => save(sources.filter((_, k) => k !== i))}>×</button>
        </div>`
      )}
      <div class="cat-url-row">
        <input
          class="mono cat-url-input"
          type="url"
          placeholder="https://example.org/catalog/index.json"
          value=${draft}
          onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
          onKeyDown=${(e: KeyboardEvent) => {
            if (e.key === 'Enter') add();
          }}
        />
        <button class="btn small" disabled=${!draft.trim()} onClick=${add}>Add</button>
      </div>
    </div>
  </div>`;
}

export function CatalogPage() {
  const s = useStore();
  const [dismissed, setDismissed] = useState('');
  useEffect(() => {
    loadCatalog().catch(() => {});
    loadConfigs().catch(() => {});
  }, []);
  // A finished job changes what the listing shows — the imported badge, the
  // images already here, a refreshed index — so reread it on every terminal
  // state, and when a refresh ends (its terminal state is idle).
  const jobState = s.catalogJob?.state || '';
  const prevState = useRef(jobState);
  useEffect(() => {
    if (prevState.current === jobState) return;
    prevState.current = jobState;
    if (['idle', 'done', 'failed', 'cancelled'].includes(jobState)) {
      loadCatalog().catch(() => {});
      loadConfigs().catch(() => {});
    }
  }, [jobState]);

  const listing = s.catalog;
  const job = s.catalogJob;
  const busy = !!job && ['starting', 'downloading', 'verifying', 'extracting', 'importing'].includes(job.state);
  const jobKey = job ? job.config + ':' + job.state : '';
  const sources = listing?.sources || [];
  const urls = sources.map((src) => src.url);

  return html`<section class="page active" data-page="catalog">
    <p class="lede">
      Machines other people have published — a configuration and the disk images it
      needs, fetched and imported by the board itself. Imported machines appear under
      Configurations.
    </p>
    ${jobVisible(job) && jobKey !== dismissed &&
    html`<${JobCard} job=${job} onDismiss=${() => setDismissed(jobKey)} />`}
    ${listing === null
      ? html`<div class="muted">Asking the board…</div>`
      : sources.length === 0
        ? html`<div class="card"><div class="card-body">
            <p style="margin-top:0">
              No catalogues are subscribed. A catalogue is a small JSON index on any web
              server, naming configuration bundles to download — add one below to see
              what it offers.
            </p>
          </div></div>`
        : sources.map((src) => html`<${SourceCard} key=${src.url} source=${src} busy=${busy} />`)}
    ${listing !== null &&
    html`<div class="cat-actions">
      <button class="btn small" disabled=${busy} onClick=${() => catalogRefresh()}>
        Refresh catalogues
      </button>
      ${listing.refreshed_at &&
      html`<span class="muted">last refreshed ${listing.refreshed_at}</span>`}
    </div>
    <${SourcesEditor} sources=${urls} />`}
  </section>`;
}
