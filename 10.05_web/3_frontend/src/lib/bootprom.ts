// The M9312's boot PROMs and the controllers they boot from. Each boot PROM
// serves one controller family and names its entry points with code labels;
// "<mnemonic>0n" boots unit 0 without running the PROM's diagnostics.
import { store } from '../store';
import { copyPackageRom, packageRoms, refreshImages, romLabels } from '../api';

export interface BootProm {
  controller: string; // the controller's type, as GET /api/devices names it
  part: string; // the PROM's part number, which names its listing file
  mnemonic: string; // the device mnemonic the PROM's labels start with
}

export const BOOT_PROMS: BootProm[] = [
  { controller: 'RL11', part: '23-751A9', mnemonic: 'dl' },
  { controller: 'RK611', part: '23-752A9', mnemonic: 'dm' },
  { controller: 'RX11', part: '23-753A9', mnemonic: 'dx' },
  { controller: 'RK11', part: '23-756A9', mnemonic: 'dk' },
  { controller: 'TS11', part: '23-764A9', mnemonic: 'ms' },
  { controller: 'UDA50', part: '23-767A9', mnemonic: 'du' },
  { controller: 'RX211', part: '23-811A9', mnemonic: 'dy' },
];

export const ROM_SOCKETS = [
  'consemu_file',
  'bootrom1_file',
  'bootrom2_file',
  'bootrom3_file',
  'bootrom4_file',
];

// What the boot logic needs of a device row of the configuration editor.
export interface BootRow {
  name: string;
  type: string;
  enabled: boolean;
  image?: string;
  params: { n: string; v: string }[];
  drives?: BootRow[];
}

export const isM9312 = (r: BootRow): boolean =>
  r.params.some((p) => p.n === 'bootrom1_file') && r.params.some((p) => p.n === 'bootaddress_label');

// The boot PROM a listing file is, by its part number.
export function promOfFile(file: string): BootProm | undefined {
  const base = file.split('/').pop() || '';
  return BOOT_PROMS.find((b) => base.toUpperCase().startsWith(b.part.toUpperCase()));
}

// The boot PROMs serving the controllers the configuration carries, each with
// whether one of its drives holds a medium.
export function configPromsOf(rows: BootRow[]): { prom: BootProm; row: BootRow; media: boolean }[] {
  const out: { prom: BootProm; row: BootRow; media: boolean }[] = [];
  for (const r of rows) {
    if (!r.enabled) continue;
    const prom = BOOT_PROMS.find((b) => b.controller === r.type);
    if (!prom) continue;
    const media = (r.drives || []).some((d) => d.enabled && !!d.image);
    out.push({ prom, row: r, media });
  }
  return out;
}

// The controller the machine boots from: the one controller with a boot PROM,
// or of several the one whose drives hold a medium. Undefined when that does
// not single one out.
export function bootController(rows: BootRow[]): BootProm | undefined {
  const all = configPromsOf(rows);
  if (all.length === 1) return all[0].prom;
  const loaded = all.filter((c) => c.media);
  return loaded.length === 1 ? loaded[0].prom : undefined;
}

// The label that boots unit 0 of a PROM's controller without diagnostics.
export const unitZeroLabel = (prom: BootProm): string => prom.mnemonic + '0n';

// The listing of a PROM as a configuration names it: a copy in the images tree
// when there is one, preferring the ROM folder, and otherwise the package's
// copied into it. Empty when neither is to be had.
export async function promFile(prom: BootProm): Promise<string> {
  await refreshImages().catch(() => {});
  const name = prom.part + '.lst';
  const copies = store.images.filter(
    (i) => (i.path.split('/').pop() || '').toUpperCase() === name.toUpperCase()
  );
  const inRoms = copies.find((i) => i.dir === 'roms');
  if (inRoms || copies.length) return (inRoms || copies[0]).path;
  const shipped = (await packageRoms()).find((r) => r.name.toUpperCase() === name.toUpperCase());
  return shipped ? copyPackageRom(shipped.name, 'roms') : '';
}

// The code labels the listings in a card's sockets define, together.
export async function socketLabels(files: string[]): Promise<string[]> {
  const sets = await Promise.all(files.filter((f) => !!f).map((f) => romLabels(f)));
  return Array.from(new Set(sets.flat())).sort();
}
