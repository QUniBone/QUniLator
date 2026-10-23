// The board's clock, set from this browser's when no time server keeps it.
//
// A BeagleBone has no clock that runs while it is off. On a network it takes
// the time from a time server; one with no way to a server keeps whatever time
// it last had, which can be weeks behind - far enough that an update file,
// signed when it was published, is refused as signed in the board's future.
// The operator's browser knows the time, so a page that has signed in hands it
// over, and says so.
import { apiJSON } from '../api';
import { toast } from './toast';

// The note says what was done to the board while the page was still loading,
// so it stays long enough to be read.
const NOTE_MS = 15 * 1000;

// A board this close to the browser is left alone: the two clocks never agree
// to the second, and a minute is well inside what anything on the board minds.
const TOLERANCE_MS = 60 * 1000;

function describe(ms: number): string {
  const min = Math.round(Math.abs(ms) / 60000);
  if (min < 120) return `${min} minutes`;
  const hours = Math.round(min / 60);
  if (hours < 48) return `${hours} hours`;
  return `${Math.round(hours / 24)} days`;
}

export async function syncBoardClock(): Promise<void> {
  const r = await apiJSON<{ time: number; synchronized: boolean }>('/api/clock');
  if (!r.ok || r.data.synchronized) return;
  const off = r.data.time - Date.now();
  if (Math.abs(off) <= TOLERANCE_MS) return;
  const set = await apiJSON('/api/clock', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ time: Date.now() }),
  });
  if (set.ok)
    toast(
      'clock',
      `the board's clock was ${describe(off)} ${off < 0 ? 'behind' : 'ahead'}; set from this browser`,
      NOTE_MS
    );
}
