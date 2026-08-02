// Dashboard grid placement: widgets occupy w×h blocks of square cells and never
// overlap. A stored position is honoured when it still fits; anything unplaced
// or colliding flows into the first free area. Pure functions, so the placement
// is deterministic and testable independent of the DOM.
import type { DashLayout } from '../types';

export interface GridItem {
  key: string;
  w: number;
  h: number;
  // Start a fresh row: the item flows below everything placed before it rather
  // than into a gap beside them. The console card takes this, so a machine with
  // no stored arrangement reads panels first, console under them.
  newRow?: boolean;
}
export interface Placed extends GridItem {
  x: number;
  y: number;
}

const cellKey = (x: number, y: number) => x + ',' + y;

// Does a w×h block at (x,y) fit within the columns and clear of occupied cells?
export function fits(
  occ: Set<string>,
  x: number,
  y: number,
  w: number,
  h: number,
  cols: number
): boolean {
  if (x < 0 || y < 0 || x + w > cols) return false;
  for (let dy = 0; dy < h; dy++)
    for (let dx = 0; dx < w; dx++) if (occ.has(cellKey(x + dx, y + dy))) return false;
  return true;
}

function mark(occ: Set<string>, x: number, y: number, w: number, h: number): void {
  for (let dy = 0; dy < h; dy++) for (let dx = 0; dx < w; dx++) occ.add(cellKey(x + dx, y + dy));
}

// The first free cell (scanning rows top-to-bottom, left-to-right) where a w×h
// block fits, starting at row `fromY`.
export function firstFree(
  occ: Set<string>,
  w: number,
  h: number,
  cols: number,
  fromY = 0
): { x: number; y: number } {
  for (let y = fromY; ; y++)
    for (let x = 0; x + w <= cols; x++) if (fits(occ, x, y, w, h, cols)) return { x, y };
}

// Resolve items into non-overlapping placements. Items with a stored position
// that still fits keep it; the rest flow into free space in item order.
export function placeItems(items: GridItem[], layout: DashLayout, cols: number): Placed[] {
  const occ = new Set<string>();
  const placed: Placed[] = [];
  const pending: GridItem[] = [];
  let bottom = 0; // the row below everything placed so far
  for (const it of items) {
    const p = layout[it.key];
    if (p && Number.isFinite(p.x) && Number.isFinite(p.y) && fits(occ, p.x, p.y, it.w, it.h, cols)) {
      mark(occ, p.x, p.y, it.w, it.h);
      bottom = Math.max(bottom, p.y + it.h);
      placed.push({ ...it, x: p.x, y: p.y });
    } else pending.push(it);
  }
  for (const it of pending) {
    const { x, y } = firstFree(occ, it.w, it.h, cols, it.newRow ? bottom : 0);
    mark(occ, x, y, it.w, it.h);
    bottom = Math.max(bottom, y + it.h);
    placed.push({ ...it, x, y });
  }
  return placed;
}

// The occupied-cell set for everything except `exceptKey`, to test a drag drop
// against without counting the dragged item itself.
export function occupancyExcept(placed: Placed[], exceptKey: string): Set<string> {
  const occ = new Set<string>();
  for (const p of placed) {
    if (p.key === exceptKey) continue;
    mark(occ, p.x, p.y, p.w, p.h);
  }
  return occ;
}

// Bottom-most occupied row, for sizing the canvas.
export function gridRows(placed: Placed[]): number {
  return placed.reduce((m, p) => Math.max(m, p.y + p.h), 0);
}
