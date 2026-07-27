import { html } from '../html';
import { useEffect, useRef } from 'preact/hooks';
import { useStore, store, emit } from '../store';
import { useQueryParam } from '../router';
import { fetchLogPage } from '../api';
import type { LogLevelName } from '../types';

// Diagnostics: the log stream with level filters, newest first. The journal is
// loaded from disk when the page opens (surviving a reload and a service
// restart) and older entries page in as the list is scrolled; the live
// /ws/events stream appends new lines on top. The active filter set is
// ephemeral view state kept in the ?levels= query so the screen reproduces from
// its URL.
export function LogPage() {
  const s = useStore();
  const [levelsQ, setLevelsQ] = useQueryParam('levels');
  const boxRef = useRef<HTMLDivElement | null>(null);
  const loading = useRef(false);

  useEffect(() => {
    if (levelsQ) {
      s.activeLevels = new Set(levelsQ.split(',').map((x) => x.toUpperCase() as LogLevelName));
    }
    // load the newest page of the journal on open; the store holds it ascending
    loading.current = true;
    fetchLogPage().then(({ entries, more }) => {
      store.log = entries.slice().reverse();
      store.logMore = more;
      loading.current = false;
      emit();
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const loadOlder = async () => {
    if (loading.current || !store.logMore || !store.log.length) return;
    loading.current = true;
    const { entries, more } = await fetchLogPage(store.log[0].id);
    // older entries, ascending, ahead of what is already held
    store.log = entries.slice().reverse().concat(store.log);
    store.logMore = more;
    loading.current = false;
    emit();
  };
  const onScroll = (e: Event) => {
    const el = e.target as HTMLDivElement;
    // newest is at the top; older pages in as the bottom is approached
    if (el.scrollTop + el.clientHeight >= el.scrollHeight - 48) loadOlder();
  };

  // newest first for display
  const rows = s.log.filter((l) => s.activeLevels.has(l.lvl)).reverse();
  const clsOf = (l: { lvl: LogLevelName }) =>
    ({ FATAL: 'err', ERROR: 'err', WARNING: 'warn', INFO: 'ok', DEBUG: 'off' })[l.lvl];
  const toggle = (lvl: LogLevelName) => {
    s.activeLevels.has(lvl) ? s.activeLevels.delete(lvl) : s.activeLevels.add(lvl);
    setLevelsQ([...s.activeLevels].map((x) => x.toLowerCase()).join(','));
    emit();
  };
  return html`<section class="page active" data-page="log">
    <div class="card logcard">
      <div class="card-head"><div class="filter-row" style="margin:0 0 0 auto">
        ${(
          [
            ['ERROR', 'err', 'error'],
            ['WARNING', 'warn', 'warning'],
            ['INFO', 'ok', 'info'],
            ['DEBUG', 'off', 'debug'],
          ] as const
        ).map(
          ([lvl, cls, label]) =>
            html`<span class=${'chip ' + cls + (s.activeLevels.has(lvl) ? '' : ' inactive')} onClick=${() =>
              toggle(lvl)}>${label}</span>`
        )}
      </div></div>
      <div class="card-body logbox" ref=${boxRef} onScroll=${onScroll}>${
        rows.length
          ? rows.map(
              (l) => html`
        <div key=${l.id} style="padding:3px 0; border-bottom:1px solid var(--line-soft)">
          <span class="muted">${l.t}</span> <span class=${'chip ' + clsOf(l)}>${l.lvl.toLowerCase()}</span>${' '}
          <span style="color:var(--accent)">${l.src}</span> ${l.msg}</div>`
            )
          : html`<div class="log-empty muted">no log entries matched by filter</div>`
      }${
        rows.length && s.logMore
          ? html`<div class="log-more muted">scroll for older entries…</div>`
          : null
      }</div>
    </div></section>`;
}
