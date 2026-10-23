// The console file senders against real receivers: lrzsz's lrx for XMODEM,
// C-Kermit for Kermit, and cat on a cooked terminal for typed text, and the
// text uploader against a simulated guest that is hard to type to. Kermit and
// cat run on a pseudo-terminal through pty-relay.py, lrx on plain pipes, and
// the file each writes is compared with the one sent.
//
//   node tools/transfer-test.mjs
//
// Needs lrzsz and C-Kermit on the PATH (Homebrew: lrzsz, c-kermit).
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ByteQueue, sendRaw, sendXmodem, sendKermit, receiveXmodem, receiveKermit } from '../src/lib/transfer.ts';

const here = dirname(fileURLToPath(import.meta.url));
const relay = join(here, 'pty-relay.py');

// A program on a terminal, as a TransferLink and the queue of what it sends.
// sevenBit strips the eighth bit from everything on the line, both ways, as a
// seven-bit line does.
// With direct set the program runs on pipes, without a terminal.
function receiver(cwd, argv, sevenBit, direct = false) {
  const cmd = direct ? argv : ['python3', relay, ...argv];
  const child = spawn(cmd[0], cmd.slice(1), { cwd, stdio: ['pipe', 'pipe', direct ? 'ignore' : 'inherit'] });
  const input = new ByteQueue();
  child.stdout.on('data', (d) => input.push(sevenBit ? new Uint8Array(d).map((b) => b & 0x7f) : new Uint8Array(d)));
  const exited = new Promise((resolve) => child.on('exit', (code) => resolve(code)));
  const link = {
    send(bytes) {
      child.stdin.write(sevenBit ? Buffer.from(bytes.map((b) => b & 0x7f)) : Buffer.from(bytes));
    },
  };
  return { link, input, exited, child };
}

function sample(size, binary) {
  const b = new Uint8Array(size);
  if (binary) for (let i = 0; i < size; i++) b[i] = (i * 7919 + (i >> 3)) & 0xff;
  else {
    const text = 'The quick brown fox # jumps & over the lazy dog ~ 0123456789\n';
    for (let i = 0; i < size; i++) b[i] = text.charCodeAt(i % text.length);
  }
  return b;
}

const quiet = () => {};
let failures = 0;
async function check(name, run) {
  const t0 = Date.now();
  try {
    await run();
    console.log(`ok   ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    failures++;
    console.log(`FAIL ${name}: ${e.message}`);
  }
}
function same(got, want) {
  if (got.length !== want.length) throw new Error(`length ${got.length}, sent ${want.length}`);
  for (let i = 0; i < want.length; i++)
    if (got[i] !== want[i]) throw new Error(`byte ${i} is ${got[i]}, sent ${want[i]}`);
}

async function xmodem(name, size, rxArgs, oneK) {
  await check(name, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xm-'));
    const data = sample(size, true);
    // the host's terminal driver takes control characters out of the input
    // even in lrx's raw mode, so XMODEM's binary blocks go over pipes
    const r = receiver(dir, ['lrx', '-q', ...rxArgs, 'out.bin'], false, true);
    await sendXmodem(r.link, r.input, data, { oneK, startTimeoutMs: 20000 }, quiet);
    await r.exited;
    // XMODEM pads the last block to its size with SUB
    const got = readFileSync(join(dir, 'out.bin'));
    same(got.subarray(0, size), data);
    if (got.subarray(size).some((b) => b !== 0x1a)) throw new Error('padding is not SUB');
  });
}

async function kermit(name, size, binary, sevenBit) {
  await check(name, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'k-'));
    const data = sample(size, binary);
    const args = ['kermit', '-Y', '-q', '-C', 'set file collision overwrite'];
    args.push(binary ? '-i' : '-T');
    args.push('-r');
    const r = receiver(dir, args, sevenBit);
    await sendKermit(r.link, r.input, data, { name: 'sample file.dat', text: !binary, startTimeoutMs: 20000 }, quiet);
    await r.exited;
    same(readFileSync(join(dir, 'sample_file.dat')), data);
  });
}

await xmodem('XMODEM checksum, 128-byte blocks', 5000, [], false);
await xmodem('XMODEM CRC, 128-byte blocks', 5000, ['-c'], false);
await xmodem('XMODEM CRC, 1K blocks', 20000, ['-c'], true);
await xmodem('XMODEM, exactly one block', 128, ['-c'], false);
await kermit('Kermit binary, eight-bit line', 20000, true, false);
await kermit('Kermit binary, seven-bit line', 20000, true, true);
await kermit('Kermit text', 5000, false, false);

const rawDefaults = {
  crForLf: true, echo: false, echoTimeoutMs: 500, fifo: 0, prompt: '', promptTimeoutMs: 2000,
  charDelayMs: 0, lineDelayMs: 0,
};

// A guest that is hard to type to. It takes 3 ms to read each character from
// its line, which holds `depth` characters besides the one being read, so one
// that arrives when those are taken is lost. It echoes what it has read, in upper case, and
// breaks the screen line every 20 columns; a line end it echoes at once, and
// then takes 40 ms over the line before it prints its prompt, losing whatever
// arrives meanwhile. `swallow` holds characters it takes without echoing or
// keeping them, as VMS does with } and what follows it on a terminal set to
// /NOLOWERCASE.
function fussyGuest(prompt, depth = 0, swallow = '') {
  const input = new ByteQueue();
  const got = [];
  const line = []; // characters waiting on the line
  let busy = false;
  let column = 0;
  const say = (str) => input.push(Uint8Array.from(Buffer.from(str)));
  const take = () => {
    if (busy || !line.length) return;
    busy = true;
    const b = line.shift();
    if (swallow.includes(String.fromCharCode(b))) {
      setTimeout(() => {
        busy = false;
        take();
      }, 3);
      return;
    }
    got.push(b === 0x0d ? 0x0a : b);
    if (b === 0x0d) {
      // the line end is echoed at once, and the line takes 40 ms
      column = 0;
      say('\r\n');
      setTimeout(() => {
        say(prompt);
        busy = false;
        take();
      }, 40);
    } else {
      setTimeout(() => {
        let echo = String.fromCharCode(b).toUpperCase();
        if (++column === 20) {
          echo += '\r\n';
          column = 0;
        }
        say(echo);
        busy = false;
        take();
      }, 3);
    }
  };
  const link = {
    send(bytes) {
      for (const b of bytes) {
        if (busy && line.length >= depth) continue; // overrun
        line.push(b);
      }
      take();
    },
  };
  return { link, input, got };
}

const fussyText = new TextEncoder().encode(
  'line one is longer than twenty columns\nline two\n\tindented line three\nlast line\n'
);

await check('typed text, all at once, loses characters', async () => {
  const g = fussyGuest('*');
  await sendRaw(g.link, g.input, fussyText, rawDefaults, quiet);
  if (g.got.length >= fussyText.length) throw new Error('the simulated guest kept up');
});

await check('typed text, echo and prompt checked', async () => {
  const g = fussyGuest('*');
  const res = await sendRaw(g.link, g.input, fussyText, { ...rawDefaults, echo: true, prompt: '*' }, quiet);
  same(Uint8Array.from(g.got), fussyText);
  if (res.unechoed || res.promptsMissed) throw new Error(JSON.stringify(res));
});

await check('typed text, echo checked, one character ahead into a buffer of one', async () => {
  const g = fussyGuest('*', 1);
  const res = await sendRaw(g.link, g.input, fussyText, { ...rawDefaults, echo: true, fifo: 1, prompt: '*' }, quiet);
  same(Uint8Array.from(g.got), fussyText);
  if (res.unechoed || res.promptsMissed) throw new Error(JSON.stringify(res));
});

for (const fifo of [0, 1]) {
  await check(`typed text, characters swallowed without an echo, ${fifo} ahead`, async () => {
    const g = fussyGuest('*', fifo, '};');
    const text = new TextEncoder().encode('braces {a}; colons a:b; done\n\tnext}; line\n');
    const res = await sendRaw(g.link, g.input, text, { ...rawDefaults, echo: true, fifo, echoTimeoutMs: 300, prompt: '*' }, quiet);
    const want = new TextDecoder().decode(text).replaceAll(';', '').replaceAll('}', '');
    same(Uint8Array.from(g.got), new TextEncoder().encode(want));
    if (res.unechoed !== 5) throw new Error(`${res.unechoed} counted without their echo, 5 swallowed`);
  });
}

await check('typed text, four characters ahead into no buffer, loses characters', async () => {
  const g = fussyGuest('*', 0);
  await sendRaw(g.link, g.input, fussyText, { ...rawDefaults, echo: true, echoTimeoutMs: 100, fifo: 4, prompt: '*', promptTimeoutMs: 200 }, quiet);
  if (g.got.length >= fussyText.length) throw new Error('the simulated guest kept up');
});

await check('typed text, prompt checked and paced, no echo check', async () => {
  const g = fussyGuest('*');
  const res = await sendRaw(g.link, g.input, fussyText, { ...rawDefaults, prompt: '*', charDelayMs: 5 }, quiet);
  same(Uint8Array.from(g.got), fussyText);
  if (res.promptsMissed) throw new Error(JSON.stringify(res));
});

async function xmodemFrom(name, size, sxArgs) {
  await check(name, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xs-'));
    const data = sample(size, true);
    writeFileSync(join(dir, 'in.bin'), data);
    const s = receiver(dir, ['lsx', '-q', ...sxArgs, 'in.bin'], false, true);
    const got = await receiveXmodem(s.link, s.input, { startTimeoutMs: 20000, stripPadding: false }, quiet);
    await s.exited;
    same(got.subarray(0, size), data);
    if (got.length % 128 || got.subarray(size).some((b) => b !== 0x1a)) throw new Error('padding is not SUB to a block');
  });
}

async function kermitFrom(name, files, binary, sevenBit) {
  await check(name, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'ks-'));
    const sent = files.map(([fname, size]) => {
      const data = sample(size, binary);
      writeFileSync(join(dir, fname), data);
      return { fname, data };
    });
    const args = ['kermit', '-Y', '-q', binary ? '-i' : '-T', '-s', ...sent.map((f) => f.fname)];
    const s = receiver(dir, args, sevenBit);
    const got = await receiveKermit(s.link, s.input, { startTimeoutMs: 20000, textToLf: !binary }, quiet);
    await s.exited;
    if (got.length !== sent.length) throw new Error(`${got.length} files, ${sent.length} sent`);
    sent.forEach((f, k) => {
      if (got[k].name.toLowerCase() !== f.fname.toLowerCase()) throw new Error(`name ${got[k].name}, sent ${f.fname}`);
      same(got[k].data, f.data);
    });
  });
}

await xmodemFrom('XMODEM from the guest, 128-byte blocks', 5000, []);
await xmodemFrom('XMODEM from the guest, 1K blocks', 20000, ['-k']);
await kermitFrom('Kermit from the guest, binary, eight-bit line', [['a.bin', 20000]], true, false);
await kermitFrom('Kermit from the guest, binary, seven-bit line', [['a.bin', 20000]], true, true);
await kermitFrom('Kermit from the guest, text', [['a.txt', 5000]], false, false);
await kermitFrom('Kermit from the guest, two files', [['one.bin', 3000], ['two.bin', 7000]], true, false);

await check('typed text, paced on the echo', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'raw-'));
  const text = sample(610, false); // ten whole lines, so one ^D ends cat's input
  const r = receiver(dir, ['sh', '-c', 'cat > out.txt'], false);
  await new Promise((res) => setTimeout(res, 300));
  const opts = { ...rawDefaults, echo: true };
  const res = await sendRaw(r.link, r.input, text, opts, quiet);
  if (res.unechoed) throw new Error(`${res.unechoed} characters without their echo`);
  r.link.send(Uint8Array.from([4]));
  await r.exited;
  same(readFileSync(join(dir, 'out.txt')), text);
});

process.exit(failures ? 1 : 0);
