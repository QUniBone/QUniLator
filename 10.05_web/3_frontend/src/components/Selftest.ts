// /system/selftest: the card's hardware self-tests.
//
// The tests are the demo program's test menus, run one at a time in the cli as
// a child of the service. The child takes the hardware like the interactive menu
// does, so the machine goes down for the length of a run and comes back
// switched off. This page is reached from the System page, not the sidebar: it
// is a workbench for checking a card out, not a place the day-to-day operator
// passes through.
//
// The output pane taps /ws/selftest, its own socket: the channel replays the
// current run's output to a page that connects mid-run, then streams live (the
// {"live":true} TEXT frame marks the seam, which a raw byte pane can ignore).
// Run state (running / verdict) comes from the store's "selftest" event.
import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import { useStore } from '../store';
import { fetchSelftests, runSelftest, stopSelftest } from '../api';
import { wsURL } from '../lib/util';
import type { SelftestInfo } from '../types';

const CATEGORIES: [string, string][] = [
  ['bus', 'Bus interface'],
  ['panel', 'Panel and card'],
  ['memory', 'Machine memory'],
];

// The acceptance-test procedure for this card: which backplane, which
// terminator, which jumpers. It is the same set of tests this page runs, so it
// is the setup instruction for them - one page per bus.
const ACCEPTANCE_DOC: Record<string, [string, string]> = {
  UNIBUS: [
    'https://retrocmp.com/projects/unibone/287-unibone-acceptance-test',
    'the UniBone acceptance test',
  ],
  QBUS: [
    'https://retrocmp.com/projects/qbone/320-qbone-acceptance-test',
    'the QBone acceptance test',
  ],
};

function verdictLabel(verdict: string): string {
  switch (verdict) {
    case 'passed':
      return 'passed';
    case 'failed':
      return 'errors found';
    case 'error':
      return 'could not run';
    case 'aborted':
      return 'aborted';
    default:
      return verdict;
  }
}

/**
 * The raw output of the running (or last) test. The bytes are terminal-ish -
 * progress bars use bare CR to redraw a line - so a lone \r discards the
 * current line and \r\n is just a line ending. The buffer lives in a ref and
 * the <pre> is written imperatively: a memory test prints tens of chunks a
 * second, and a re-render per chunk would thrash the page.
 */
function OutputPane({ onData }: { onData: () => void }) {
  const preRef = useRef<HTMLPreElement>(null);
  const bufRef = useRef({ text: '', pendingCR: false });
  const onDataRef = useRef(onData);
  onDataRef.current = onData;
  useEffect(() => {
    let ws: WebSocket | null = null;
    let closed = false;
    const paint = () => {
      const pre = preRef.current;
      if (!pre) return;
      pre.textContent = bufRef.current.text || 'No output yet: run a test.';
      pre.scrollTop = pre.scrollHeight;
    };
    const feed = (chunk: string) => {
      const b = bufRef.current;
      for (const ch of chunk) {
        if (b.pendingCR) {
          b.pendingCR = false;
          // a lone CR redraws the line; CRLF is only a line ending
          if (ch !== '\n') b.text = b.text.slice(0, b.text.lastIndexOf('\n') + 1);
        }
        if (ch === '\r') {
          b.pendingCR = true;
          continue;
        }
        b.text += ch;
      }
      // bound what one long soak test can pin in the page
      if (b.text.length > 256 * 1024) b.text = b.text.slice(-192 * 1024);
      paint();
      // the pane is folded away until there is something in it - which is the
      // channel's replay as much as a live run, so the pane says so, not the page
      onDataRef.current();
    };
    const connect = () => {
      if (closed) return;
      ws = new WebSocket(wsURL('/ws/selftest'));
      ws.binaryType = 'arraybuffer';
      ws.onmessage = (e) => {
        // TEXT frames are the channel's control messages ({"live":true}); the
        // test output itself is binary
        if (typeof e.data === 'string') return;
        // a fresh replay follows a cleared ring: drop what a previous run left
        feed(new TextDecoder().decode(new Uint8Array(e.data as ArrayBuffer)));
      };
      ws.onclose = () => {
        if (!closed) setTimeout(connect, 2000);
      };
    };
    connect();
    paint();
    return () => {
      closed = true;
      ws?.close();
    };
  }, []);
  return html`<pre class="upd-changelog selftest-output" ref=${preRef}></pre>`;
}

function TestRow({ t, busy }: { t: SelftestInfo; busy: boolean }) {
  const [seconds, setSeconds] = useState(String(t.default_seconds));
  return html`<div class="selftest-row">
    <div class="selftest-row-text">
      <div class="selftest-row-head"><strong>${t.label}</strong>
        <span class="muted mono">${t.id}</span>
        ${
          t.machine_safe
            ? html`<span class="chip ok" title="this one may be run with the card in a machine"
                >machine safe</span>`
            : null
        }</div>
      <div class="muted">${t.description}</div>
      ${t.warning ? html`<div class="selftest-warning">${t.warning}</div>` : null}
      ${
        t.setup
          ? html`<div class="selftest-setup"><strong>Before the run:</strong> ${t.setup}</div>`
          : null
      }
    </div>
    <div class="selftest-row-run">
      ${
        t.unbounded
          ? html`<label class="muted">seconds
              <input class="selftest-seconds" type="number" min="0" max="86400"
                value=${seconds} disabled=${busy}
                onInput=${(e: Event) => setSeconds((e.target as HTMLInputElement).value)} />
            </label>`
          : null
      }
      <button class="btn small" disabled=${busy}
        onClick=${() => runSelftest(t.id, t.unbounded ? Number(seconds) || 0 : 0)}>Run</button>
    </div>
  </div>`;
}

export function SelftestPage() {
  const s = useStore();
  const [tests, setTests] = useState<SelftestInfo[] | null>(null);
  // whether the pane has anything in it: the card is a header-high strip until
  // it has, so an idle page spends its height on the tests
  const [hasOutput, setHasOutput] = useState(false);
  useEffect(() => {
    fetchSelftests().then((r) => setTests(r ? r.tests : []));
  }, []);

  const running = s.selftest.running;
  const last = s.selftest.last;
  const busy = running !== null;
  const doc = ACCEPTANCE_DOC[s.platform];
  const hint = (running ? running.hint : last ? last.hint : '') || '';

  // The output is docked to the bottom of the page and the tests scroll behind
  // it: a run is watched, not scrolled to, and Stop stays under the hand
  // wherever in the list the test that is running was started from.
  return html`<section class="page active" data-page="selftest">
    <div class="selftest-scroll">
    <div class="selftest-danger" role="alert" style="max-width:720px">
      <div class="selftest-danger-head">Never run these tests in a real machine</div>
      <p>
        Except the ones marked${' '}<span class="chip ok">machine safe</span>, these
        tests are for a card on the bench, in an${' '}<strong>empty, terminated
        backplane</strong>. They switch the card's bus drivers on and put arbitrary
        patterns on the address, data, control and grant lines: with cards or a live
        CPU in the backplane that is traffic nothing can make sense of, and the
        latch tests short the grant chain with loopback jumpers on top of it.
      </p>
      ${
        doc
          ? html`<p>What the card needs to be sitting in - backplane, terminator
              and jumpers - is${' '}<a href=${doc[0]} target="_blank" rel="noreferrer"
                >${doc[1]}</a>.</p>`
          : null
      }
    </div>

    ${
      tests === null
        ? html`<p class="muted">Reading the test catalog…</p>`
        : CATEGORIES.filter(([cat]) => tests.some((t) => t.category === cat)).map(
            ([cat, title]) => html`<div class="card" style="max-width:720px">
              <div class="card-head"><h3>${title}</h3></div>
              <div class="card-body">
                ${tests
                  .filter((t) => t.category === cat)
                  .map((t) => html`<${TestRow} t=${t} busy=${busy} />`)}
              </div>
            </div>`
          )
    }
    </div>

    <div class=${'card selftest-output-card' + (hasOutput ? ' has-output' : '')
        + (busy ? ' running' : '')}>
      <div class="card-head"><h3>Output</h3>
        ${
          running
            ? html`<span class="pill busy"><span class="btn-spinner" aria-hidden="true"></span>
                running ${running.test}</span>
              <button class="btn small danger" onClick=${() => stopSelftest()}>Stop</button>`
            : last
              ? html`<span class=${'pill selftest-verdict-' + last.verdict}>
                  ${last.test}: ${verdictLabel(last.verdict)}</span>`
              : html`<span class="muted">nothing has run yet</span>`
        }
      </div>
      <div class="card-body">
        ${
          // What the test made of its own failure: the missing loopback jumpers
          // produce an exact set of errors, and the operator should not have to
          // read eight signal paths to recognise it.
          hint
            ? html`<div class="selftest-hint"><strong>Likely cause:</strong> ${hint}</div>`
            : null
        }
        <${OutputPane} onData=${() => setHasOutput(true)} />
        ${
          last && !running
            ? html`<p class="muted">The machine is back and switched off; AUX ON on
                the dashboard brings it up.</p>`
            : null
        }
      </div>
    </div>
  </section>`;
}
