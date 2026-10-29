// Files to and from the guest through its console: sent as typed text, by
// XMODEM or by Kermit, and received by XMODEM or Kermit. lib/transfer.ts
// carries the protocols; this is the dialog the console card opens.
import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import { createPortal } from 'preact/compat';
import { store } from '../store';
import { consoleSource, sendConsoleBytes, setTransferTap, focusConsole, saveFile } from '../lib/terminals';
import {
  ByteQueue,
  TransferError,
  sendRaw,
  sendXmodem,
  sendKermit,
  receiveXmodem,
  receiveKermit,
  kermitName,
} from '../lib/transfer';
import type { RawOptions, TransferProgress } from '../lib/transfer';
import { humanSize } from '../lib/util';

export type Direction = 'send' | 'receive';
type SendProtocol = 'text' | 'xmodem' | 'kermit';
type ReceiveProtocol = 'xmodem' | 'kermit';

// A guest has a minute to start its side of the transfer.
const START_TIMEOUT_MS = 60000;
// A transfer that went well says so, and the dialog closes on its own after this.
const CLOSE_AFTER_MS = 5000;

// Whether the console line carries seven bits, which XMODEM's binary blocks do
// not survive: the VAX console unless console_8bit is set, a DL11 in a 7-bit
// mode, and the external console when its format is 7bit.
function sevenBitLine(): boolean {
  const src = consoleSource();
  const param = (type: string, name: string) =>
    store.devmodel.find((d) => d.type === type && d.enabled)?.params.find((p) => p.n === name)?.v;
  if (src === 'vax') return !/^(1|true)$/i.test(param('VAX-11/780', 'console_8bit') || '');
  if (src === 'dl11') return /^7/.test(store.devmodel.find((d) => d.name === 'DL11')?.params.find((p) => p.n === 'mode')?.v || '');
  if (src === 'ttys2') return (store.settings.external_console || {}).format === '7bit';
  return false;
}

// A file is taken for text when it holds no NUL and no byte past 127.
const looksText = (b: Uint8Array) => !b.some((x) => x === 0 || x > 127);

const num = (v: string, dflt: number) => {
  const n = parseInt(v, 10);
  return Number.isFinite(n) && n >= 0 ? n : dflt;
};

// The protocols and settings last used, so the next file goes the same way.
// Kept in this browser only, and the dialog works without them.
interface SavedSettings {
  protocol: SendProtocol;
  crForLf: boolean;
  echo: boolean;
  echoTimeout: string;
  fifo: string;
  prompt: string;
  promptTimeout: string;
  charDelay: string;
  lineDelay: string;
  oneK: boolean;
  rxProtocol: ReceiveProtocol;
  stripPadding: boolean;
  rxTextToLf: boolean;
}
const SETTINGS_KEY = 'sendfile.settings';
const DEFAULTS: SavedSettings = {
  protocol: 'text', crForLf: true, echo: true, echoTimeout: '1000', fifo: '1',
  prompt: '', promptTimeout: '5000', charDelay: '0', lineDelay: '0', oneK: false,
  rxProtocol: 'kermit', stripPadding: true, rxTextToLf: false,
};
function loadSettings(): SavedSettings {
  try {
    return { ...DEFAULTS, ...JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{}') };
  } catch {
    return DEFAULTS;
  }
}
function saveSettings(v: SavedSettings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(v));
  } catch {
    /* the settings are a convenience */
  }
}

// `initial` is a file dropped on the console, which the dialog opens with.
export function TransferDialog({
  direction,
  initial,
  onClose,
}: {
  direction: Direction;
  initial: File | null;
  onClose: () => void;
}) {
  const saved = useRef(loadSettings()).current;
  const [file, setFile] = useState<{ name: string; data: Uint8Array } | null>(null);
  const [protocol, setProtocol] = useState<SendProtocol>(saved.protocol);
  // typed text
  const [crForLf, setCrForLf] = useState(saved.crForLf);
  const [echo, setEcho] = useState(saved.echo);
  const [echoTimeout, setEchoTimeout] = useState(saved.echoTimeout);
  const [fifo, setFifo] = useState(saved.fifo);
  const [prompt, setPrompt] = useState(saved.prompt);
  const [promptTimeout, setPromptTimeout] = useState(saved.promptTimeout);
  const [charDelay, setCharDelay] = useState(saved.charDelay);
  const [lineDelay, setLineDelay] = useState(saved.lineDelay);
  // sending by XMODEM and Kermit
  const [oneK, setOneK] = useState(saved.oneK);
  const [guestName, setGuestName] = useState('');
  const [kermitTextMode, setKermitTextMode] = useState(true);
  // receiving
  const [rxProtocol, setRxProtocol] = useState<ReceiveProtocol>(saved.rxProtocol);
  const [rxName, setRxName] = useState('xmodem.bin');
  const [stripPadding, setStripPadding] = useState(saved.stripPadding);
  const [rxTextToLf, setRxTextToLf] = useState(saved.rxTextToLf);
  // the transfer
  const [progress, setProgress] = useState<TransferProgress | null>(null);
  const [outcome, setOutcome] = useState<{ ok: boolean; text: string } | null>(null);
  const [title, setTitle] = useState('');
  const abort = useRef<AbortController | null>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  // the dialog shows the settings, then the transfer as it runs, then how it went
  const view = progress == null ? 'settings' : outcome == null ? 'running' : 'done';
  const running = view === 'running';

  const load = async (f: File) => {
    const data = new Uint8Array(await f.arrayBuffer());
    setFile({ name: f.name, data });
    setGuestName(kermitName(f.name));
    setKermitTextMode(looksText(data));
  };
  const pick = (ev: Event) => {
    const input = ev.target as HTMLInputElement;
    const f = input.files?.[0];
    if (f) load(f);
    input.value = '';
  };
  useEffect(() => {
    if (initial) load(initial);
  }, [initial]);
  // with a file chosen, Return sends it the way the dialog is set
  const goButton = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (file || direction === 'receive') goButton.current?.focus();
  }, [file, direction]);

  const close = () => {
    if (running) return;
    onClose();
    setTimeout(focusConsole, 0);
  };
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') close();
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  });
  useEffect(() => {
    if (!outcome?.ok) return;
    const t = setTimeout(close, CLOSE_AFTER_MS);
    return () => clearTimeout(t);
  }, [outcome]);
  const back = () => {
    setProgress(null);
    setOutcome(null);
  };

  // Hold the console line for one transfer: what the guest sends goes to the
  // transfer, and the screen shows it only for typed text, where it is the echo.
  // With xonxoff the board keeps to the guest's XON/XOFF, which typed text and
  // Kermit never carry as data and XMODEM's blocks do.
  const run = async (heading: string, shows: boolean, xonxoff: boolean, total: number, body: (link: { send(b: Uint8Array): void }, input: ByteQueue, signal: AbortSignal) => Promise<string>) => {
    saveSettings({
      protocol, crForLf, echo, echoTimeout, fifo, prompt, promptTimeout, charDelay, lineDelay, oneK,
      rxProtocol, stripPadding, rxTextToLf,
    });
    const ctrl = new AbortController();
    abort.current = ctrl;
    const input = new ByteQueue(ctrl.signal);
    const link = {
      send(bytes: Uint8Array) {
        if (!sendConsoleBytes(bytes)) throw new TransferError('the console is not connected');
      },
    };
    setTransferTap((d) => input.push(d), shows, xonxoff);
    setTitle(heading);
    setOutcome(null);
    setProgress({ sent: 0, total, note: 'starting' });
    try {
      const text = await body(link, input, ctrl.signal);
      setOutcome({ ok: true, text });
    } catch (e) {
      setOutcome({ ok: false, text: e instanceof TransferError ? 'Stopped: ' + e.message + '.' : String(e) });
    } finally {
      setTransferTap(null, true);
      abort.current = null;
    }
  };

  const send = () => {
    if (!file) return;
    const how = protocol === 'text' ? 'as typed text' : protocol === 'xmodem' ? 'by XMODEM' : 'by Kermit';
    run(`Sending ${file.name} ${how}`, protocol === 'text', protocol !== 'xmodem', file.data.length, async (link, input, signal) => {
      if (protocol === 'text') {
        const opts: RawOptions = {
          crForLf,
          echo,
          echoTimeoutMs: num(echoTimeout, 1000),
          fifo: num(fifo, 1),
          prompt,
          promptTimeoutMs: num(promptTimeout, 5000),
          charDelayMs: num(charDelay, 0),
          lineDelayMs: num(lineDelay, 0),
        };
        const r = await sendRaw(link, input, file.data, opts, setProgress, signal);
        const notes: string[] = [];
        if (r.unechoed) notes.push(`${r.unechoed} characters went on without their echo`);
        if (r.promptsMissed) notes.push(`${r.promptsMissed} lines went on without the prompt`);
        return `${file.name} sent.` + (notes.length ? ' ' + notes.join('; ') + '.' : '');
      }
      if (protocol === 'xmodem') {
        await sendXmodem(link, input, file.data, { oneK, startTimeoutMs: START_TIMEOUT_MS }, setProgress, signal);
        return `${file.name} sent by XMODEM.`;
      }
      const opts = { name: guestName || kermitName(file.name), text: kermitTextMode, startTimeoutMs: START_TIMEOUT_MS };
      await sendKermit(link, input, file.data, opts, setProgress, signal);
      return `${file.name} sent by Kermit as ${opts.name}.`;
    });
  };

  const receive = () => {
    run(`Receiving by ${rxProtocol === 'xmodem' ? 'XMODEM' : 'Kermit'}`, false, rxProtocol === 'kermit', 0, async (link, input, signal) => {
      if (rxProtocol === 'xmodem') {
        const data = await receiveXmodem(link, input, { startTimeoutMs: START_TIMEOUT_MS, stripPadding }, setProgress, signal);
        const name = rxName.trim() || 'xmodem.bin';
        saveFile(name, new Blob([data]));
        return `${name} received by XMODEM, ${humanSize(data.length)}.`;
      }
      const files = await receiveKermit(link, input, { startTimeoutMs: START_TIMEOUT_MS, textToLf: rxTextToLf }, setProgress, signal);
      for (const f of files) saveFile(f.name, new Blob([f.data]));
      if (files.length === 1) return `${files[0].name} received by Kermit, ${humanSize(files[0].data.length)}.`;
      return `${files.length} files received by Kermit: ${files.map((f) => f.name).join(', ')}.`;
    });
  };

  const sevenBit = sevenBitLine();
  const sevenBitWarning = html` <strong>This console line carries seven bits, and XMODEM needs eight:
    use Kermit, or switch the line to eight bits.</strong>`;
  const field = (label: string, control: unknown, info?: string) =>
    html`<label class="set-name">${label}</label><div class="set-val">${control}</div>
      ${info ? html`<div class="set-info">${info}</div>` : null}`;
  const numberInput = (value: string, set: (v: string) => void, unit: string) =>
    html`<span class="sf-unit"><input type="text" inputmode="numeric" class="sf-num" value=${value}
      onInput=${(e: Event) => set((e.target as HTMLInputElement).value)} /><span class="muted">${unit}</span></span>`;
  const check = (on: boolean, set: (v: boolean) => void, label: string) =>
    html`<label class="radio"><input type="checkbox" checked=${on}
      onChange=${(e: Event) => set((e.target as HTMLInputElement).checked)} />${label}</label>`;
  const textInput = (value: string, set: (v: string) => void, cls = '') =>
    html`<input type="text" class=${cls} value=${value} onInput=${(e: Event) => set((e.target as HTMLInputElement).value)} />`;

  const sendForm = html`<div class="set-grid">
      ${field('File', html`<input type="file" hidden ref=${fileInput} onChange=${pick} />
        ${file && html`<span class="mono">${file.name}</span><span class="muted">${humanSize(file.data.length)}</span>`}
        <button class="btn small" onClick=${() => fileInput.current?.click()}>${file ? 'Choose another…' : 'Choose a file…'}</button>`)}
      ${field('Send as', html`<select value=${protocol}
          onChange=${(e: Event) => setProtocol((e.target as HTMLSelectElement).value as SendProtocol)}>
          <option value="text">Typed text</option>
          <option value="xmodem">XMODEM</option>
          <option value="kermit">Kermit</option>
        </select>`)}
      ${protocol === 'text' && html`
        ${field('Line ends', check(crForLf, setCrForLf, 'as Return (CR)'))}
        ${field('Echo', html`${check(echo, setEcho, 'wait for each character')}
          ${numberInput(echoTimeout, setEchoTimeout, 'ms at most')}`,
          'Upper case for lower, and line breaks the guest adds to a long line, count as the echo.')}
        ${echo && field('Receive buffer', html`${numberInput(fifo, setFifo, 'characters sent ahead of the echo')}`,
          '0 for a line with only its receive register; a line end waits for every echo.')}
        ${field('Prompt', html`${textInput(prompt, setPrompt, 'sf-prompt')}
          ${numberInput(promptTimeout, setPromptTimeout, 'ms at most')}`,
          'After each line, wait until the guest prints this again. Empty for none.')}
        ${field('Delay', html`${numberInput(charDelay, setCharDelay, 'ms per character')}
          ${numberInput(lineDelay, setLineDelay, 'ms per line')}`)}`}
      ${protocol === 'xmodem' && field('Blocks', check(oneK, setOneK, '1K blocks, when the receiver asks for CRC'))}
      ${protocol === 'kermit' && html`
        ${field('Name', textInput(guestName, setGuestName))}
        ${field('Type', check(kermitTextMode, setKermitTextMode, 'text, with the guest’s line ends'))}`}
    </div>
    ${protocol === 'xmodem' && html`<p class="sf-hint">Start the receiver on the guest, such as
      <code>rx FILE</code>, before or just after Send: the transfer waits a minute for it.
      ${sevenBit && sevenBitWarning}</p>`}
    ${protocol === 'kermit' && html`<p class="sf-hint">Start Kermit on the guest and give it
      <code>RECEIVE</code>, or put it in server mode, before or just after Send: the transfer
      waits a minute for it. Kermit carries eight-bit files over a seven-bit line.</p>`}
    ${protocol === 'text' && html`<p class="sf-hint">The text goes to the guest as if typed at
      the console, into whatever reads it there.</p>`}`;

  const receiveForm = html`<div class="set-grid">
      ${field('Receive by', html`<select value=${rxProtocol}
          onChange=${(e: Event) => setRxProtocol((e.target as HTMLSelectElement).value as ReceiveProtocol)}>
          <option value="kermit">Kermit</option>
          <option value="xmodem">XMODEM</option>
        </select>`)}
      ${rxProtocol === 'xmodem' && html`
        ${field('Save as', textInput(rxName, setRxName))}
        ${field('Padding', check(stripPadding, setStripPadding, 'take the ^Z filling the last block off the end'))}`}
      ${rxProtocol === 'kermit' && field('Line ends', check(rxTextToLf, setRxTextToLf, 'text files: CR LF to LF'))}
    </div>
    ${rxProtocol === 'xmodem' && html`<p class="sf-hint">Start the sender on the guest, such as
      <code>sx FILE</code>, before or just after Receive: the transfer waits a minute for it.
      XMODEM carries no name, so the file is saved under the one given.
      ${sevenBit && sevenBitWarning}</p>`}
    ${rxProtocol === 'kermit' && html`<p class="sf-hint">Start Kermit on the guest and have it
      <code>SEND</code> the files, before or just after Receive: the transfer waits a minute for
      it. Each file is saved under the name the guest sends.</p>`}`;

  const heading = direction === 'send' ? 'Send a file to the guest' : 'Receive files from the guest';
  const pct = progress && progress.total ? Math.round((progress.sent / progress.total) * 100) : 0;

  const settingsView = html`
    <div class="card-body">
      ${direction === 'send' ? sendForm : receiveForm}
      <div class="sf-actions">
        <button class="btn" onClick=${close}>Close</button>
        ${direction === 'send'
          ? html`<button class="btn primary" ref=${goButton} disabled=${!file} onClick=${send}>Send</button>`
          : html`<button class="btn primary" ref=${goButton} onClick=${receive}>Receive</button>`}
      </div>
    </div>`;

  const progressView = progress && html`
    <div class="card-body">
      <div class="sf-progress">
        ${progress.total ? html`<progress max="100" value=${pct}></progress>` : html`<progress></progress>`}
        <div class="muted">${progress.total ? `${humanSize(progress.sent)} of ${humanSize(progress.total)}` : humanSize(progress.sent)}
          — ${progress.note}</div>
      </div>
      <div class="sf-actions">
        <button class="btn" onClick=${() => abort.current?.abort()}>Cancel</button>
      </div>
    </div>`;

  const doneView = outcome && html`
    <div class="card-body">
      <p class=${'sf-outcome' + (outcome.ok ? ' good' : ' bad')}>${outcome.text}</p>
      ${outcome.ok && html`<p class="sf-hint">This closes by itself in a few seconds.</p>`}
      <div class="sf-actions">
        ${!outcome.ok && html`<button class="btn" onClick=${back}>Back</button>`}
        <button class="btn primary" ref=${(el: HTMLButtonElement | null) => el?.focus()} onClick=${close}>Close</button>
      </div>
    </div>`;

  return createPortal(
    html`<div class="modal-overlay" onClick=${(e: Event) => e.target === e.currentTarget && close()}>
    <div class="card modal-card sf-card" role="dialog" aria-label=${heading}>
      <div class="card-head"><h3>${view === 'settings' ? heading : title}</h3>
        <button class="modal-close" aria-label="Close" title="Close" disabled=${running} onClick=${close}>×</button>
      </div>
      ${view === 'settings' ? settingsView : view === 'running' ? progressView : doneView}
    </div>
  </div>`,
    document.body
  );
}
