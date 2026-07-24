import { html } from '../html';
import { useEffect, useRef } from 'preact/hooks';
import { useStore, emit } from '../store';
import { useQueryParam } from '../router';
import type { LogLevelName } from '../types';

// Diagnostics: the live log stream with level filters. The active filter set is
// ephemeral view state, kept in the ?levels= query so the screen reproduces
// from its URL.
export function LogPage() {
  const s = useStore();
  const [levelsQ, setLevelsQ] = useQueryParam('levels');
  const boxRef = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    if (levelsQ) {
      s.activeLevels = new Set(levelsQ.split(',').map((x) => x.toUpperCase() as LogLevelName));
      emit();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const rows = s.log.filter((l) => s.activeLevels.has(l.lvl));
  const clsOf = (l: { lvl: LogLevelName }) =>
    ({ FATAL: 'err', ERROR: 'err', WARNING: 'warn', INFO: 'ok', DEBUG: 'off' })[l.lvl];
  const toggle = (lvl: LogLevelName) => {
    s.activeLevels.has(lvl) ? s.activeLevels.delete(lvl) : s.activeLevels.add(lvl);
    setLevelsQ(
      [...s.activeLevels].map((x) => x.toLowerCase()).join(',')
    );
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
      <div class="card-body logbox" ref=${boxRef}>${rows.map(
        (l, i) => html`
        <div key=${i} style="padding:3px 0; border-bottom:1px solid var(--line-soft)">
          <span class="muted">${l.t}</span> <span class=${'chip ' + clsOf(l)}>${l.lvl.toLowerCase()}</span>${' '}
          <span style="color:var(--accent)">${l.src}</span> ${l.msg}</div>`
      )}</div>
    </div></section>`;
}
