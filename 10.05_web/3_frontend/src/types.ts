// Shapes exchanged with the backend (10.05_web/docs/api.md) and the derived
// "live model" the views consume.

// ---- REST: GET /api/devices ----
export type ParamType =
  | 'string'
  | 'bool'
  | 'unsigned'
  | 'unsigned64'
  | 'double'
  | 'enum';

export interface ApiParam {
  name: string;
  shortname?: string;
  type: ParamType;
  value: unknown;
  base?: number;
  bitwidth?: number;
  readonly?: boolean;
  info?: string;
  unit?: string;
  values?: string[];
  options?: string[];
  // What the value names, when it is not an ordinary one: a file of the image
  // tree. "image" is a medium a drive holds, "rom" the file a PROM card is
  // programmed from. The device declares it, so the interface offers the file
  // browser for such a parameter whatever the device called it.
  content?: ParamContent;
}

export type ParamContent = 'image' | 'rom';

// The verbal runtime state of a disk drive (GET /api/devices → status).
export type DiskStatus =
  | 'off'
  | 'idle'
  | 'spinning up'
  | 'spinning down'
  | 'loaded'
  | 'ready'
  | 'busy';

export interface ApiDevice {
  name: string;
  type: string;
  label?: string;
  category?: string;
  enabled: boolean;
  parent?: string;
  removable?: boolean;
  locked?: boolean;
  status?: DiskStatus;
  // standard bus placements for this device's type (raw numeric values), the
  // menu the config editor offers for base_addr / intr_vector
  address_options?: number[];
  vector_options?: number[];
  params: ApiParam[];
  // Machine-driven running state (lamps, LEDs, drive state machine, counters).
  // Read-only for display; absent on the pre-split backend.
  statusparams?: ApiParam[];
}

// ---- derived live model ----
export type LiveParamKind = 'oct' | 'hex' | 'uint' | 'dbl' | 'str' | 'enum' | 'bool';

export interface LiveParam {
  n: string; // name
  s?: string; // shortname
  ro?: boolean; // readonly
  i: string; // info
  u: string; // unit
  t: LiveParamKind;
  v: string; // display value
  bw?: number; // bitwidth (octal)
  opts?: string[]; // enum options
  c?: ParamContent; // names a file of the image tree (medium / ROM)
}

export interface LiveDev {
  name: string;
  type: string;
  label?: string; // friendly "<role> (<code>)" from GET /api/devices
  category?: string;
  enabled: boolean;
  removable?: boolean;
  locked?: boolean;
  status?: DiskStatus; // verbal disk state from the backend, when present
  info: string;
  params: LiveParam[]; // configuration parameters (operator/setup, editable)
  statusParams: LiveParam[]; // machine-driven running state (lamps, LEDs, counters)
  drives: LiveDev[];
  img: string;
  activity?: boolean;
  addressOptions?: number[]; // standard base_addr values for this device's type
  vectorOptions?: number[]; // standard intr_vector values for this device's type
}

// ---- REST: settings ----
export interface ExternalConsole {
  source: 'ttys2' | 'webserial' | 'off';
  port?: string;
  baud?: number;
  // "8bit" (default) or "7bit" — a 7-bit-era guest whose cooked output
  // carries even parity gets clean text by stripping bit 7 on input
  format?: string;
}
export interface Settings {
  platform: string;
  address_width: number;
  external_console: ExternalConsole;
}

// ---- REST: images / configs ----
export interface ImageUse {
  config: string;
  device: string;
}
export interface ImageInfo {
  name: string;
  path: string; // images-root-relative subpath, e.g. "du/foo.dsk"
  dir: string; // parent folder subpath; "" for the root
  size: number;
  writable?: boolean;
  mtime?: string;
  attached?: string[];
  used?: ImageUse[];
  // A copy-on-write overlay captures every write since the base image; the base
  // file itself is never touched. When active, the block/byte fields report how
  // much the overlay holds (dirty 512-byte blocks and the sidecar's real
  // on-disk footprint).
  overlay?: boolean;
  overlay_dirty_blocks?: number;
  overlay_bytes?: number;
}

// GET /api/roms — the M9312 PROM listings the package ships, offered as a
// source to copy from. They are not part of the image tree and nothing
// references them by path: the operator copies one into images/roms and owns
// the copy. `title` is the listing's ".title" line, empty if it has none.
export interface PackageRom {
  name: string;
  size: number;
  title: string;
}

// GET /api/images now returns a folder tree: the flat list of folder subpaths
// alongside the files.
export interface ImageListing {
  dirs: string[];
  images: ImageInfo[];
}

// GET /api/images/<subpath>/contents — a read-only listing of the files inside
// a disk/tape image. Field shapes vary by filesystem; the recognized values are
// "RT-11" and "ODS-2", everything else reports "foreign" or "unknown".
export interface ImageContentFile {
  name: string;
  // RT-11
  bytes?: number;
  blocks?: number;
  date?: string;
  // ODS-2
  directory?: string;
  size_bytes?: number | null;
  blocks_on_volume?: number;
  created?: string;
}
export interface ImageContents {
  file?: string;
  filesystem: string; // "RT-11" | "ODS-2" | "foreign" | "unknown"
  image_size?: number;
  home?: { volume_name?: string; [k: string]: unknown };
  files?: ImageContentFile[];
  error?: string; // set by the frontend when the fetch itself fails
}

// One row of the master list (GET /api/configs → configs[]).
export interface ConfigSummary {
  name: string;
  title?: string; // operator-friendly title; falls back to the name
  mtime?: string;
  enabled?: string[];
  dip_value?: number; // the DIP setting that selects it at power-on; -1 for none
  // whether the board switches this machine on by itself when it loads it,
  // instead of holding it dark for the panel switch
  autostart?: boolean;
}
export interface ConfigDeviceSnapshot {
  name: string;
  enabled: boolean;
  params?: Record<string, string>;
}
// How a dashboard card is shown: its top-left grid cell, whether it is hidden,
// and the switchable parts of its widget the operator has turned on or off.
// This is display state throughout — it says how a device appears on the
// dashboard, never what the device does, which is why it is stored beside the
// configuration's device set rather than among a device's parameters.
export interface WidgetPlace {
  x: number;
  y: number;
  hidden?: boolean;
  opts?: Record<string, boolean>;
}
export type DashLayout = Record<string, WidgetPlace>;

// The full document of one configuration (GET /api/configs/<name> and the
// body of PUT /api/configs/<name>).
export interface ConfigSnapshot {
  title?: string;
  layout?: DashLayout;
  devices: ConfigDeviceSnapshot[];
}

// ---- log lines (assembled from /ws/events) ----
export type LogLevelName = 'FATAL' | 'ERROR' | 'WARNING' | 'INFO' | 'DEBUG';
export interface LogLine {
  id: number; // monotonic journal id, for ordering, dedup and paging
  t: string; // server clock HH:MM:SS
  lvl: LogLevelName;
  src: string;
  msg: string;
}

// ---- self-update: GET /api/update, and the "update" event frame ----
export type UpdateState =
  | 'idle'
  | 'checking'
  | 'ahead' // the repository offers an older version than the board runs
  | 'downloading'
  | 'installing'
  | 'verifying'
  | 'os-upgrading'
  | 'done'
  | 'failed'
  | 'rolled-back';

export interface OsPackageUpdate {
  name: string;
  from: string;
  to: string;
}

export interface OsUpdates {
  count: number;
  packages: OsPackageUpdate[];
  held_back: string[];
  reboot_required: boolean;
}

export interface UpdateOutcome {
  state?: UpdateState;
  from?: string;
  to?: string;
  at?: string;
}

export interface UpdateStatus {
  state: UpdateState;
  package: string;
  source_configured: boolean;
  checked_at: string;
  installed: string;
  candidate: string;
  rollback: boolean; // a cached package the board could step back to
  needs_repair: boolean; // a dpkg interrupted by a power loss
  dismissed: string;
  os: OsUpdates;
  last: UpdateOutcome;
  error: string;
  journal: string[];
}

// ---- catalogues: GET /api/catalog and the "catalog" event frame ----
// A catalogue is a static index the board subscribes to; the board fetches
// the chosen .qcfg.zip itself and imports it, publishing its progress as the
// job below.

export type CatalogJobState =
  | 'idle'
  | 'starting'
  | 'refreshing'
  | 'downloading'
  | 'verifying'
  | 'extracting'
  | 'importing'
  | 'done'
  | 'failed'
  | 'cancelled';

export interface CatalogJob {
  state: CatalogJobState;
  mode: '' | 'refresh' | 'fetch';
  source: string;
  entry: string;
  config: string; // the name the machine is being imported under
  title: string;
  bytes_done: number;
  bytes_total: number; // the download's, then the current image's while extracting
  file: string; // the image being extracted
  files_done: number;
  files_total: number;
  images_written: number;
  images_kept: string[];
  error: string;
  note: string;
  autostart_note: string;
}

export interface CatalogImage {
  path: string;
  bytes?: number;
}

// an index entry (qunilator-catalog/1), decorated by the board with what
// only it knows
export interface CatalogEntry {
  id: string;
  title?: string;
  summary?: string;
  bus?: string; // 'qbus' | 'unibus' | 'any'
  cpu?: string; // what the backplane must carry, for the operator to judge
  devices?: string[];
  guest?: string;
  page?: string; // the entry's documentation page
  download?: { url: string; bytes: number; sha256: string };
  images?: CatalogImage[];
  doc?: { added?: string };
  imported: boolean; // a configuration of this id's name exists here
  bus_ok: boolean; // the entry's bus matches this board
  images_present: number;
  images_total: number;
}

export interface CatalogIndex {
  schema?: string;
  name?: string;
  updated?: string;
  configurations: CatalogEntry[];
}

export interface CatalogSource {
  url: string;
  ok: boolean;
  error: string;
  fetched_at?: string;
  index?: CatalogIndex; // last good content; stale when ok is false
}

export interface CatalogListing {
  refreshed_at: string;
  bus: string;
  sources: CatalogSource[];
  job: CatalogJob;
}

// ---- hardware self-tests ----
// GET /api/selftest and the "selftest" event frame. The tests run in the cli
// as a child of the service; their output streams on /ws/selftest.

export interface SelftestInfo {
  id: string;
  label: string;
  category: string; // 'bus' | 'panel' | 'memory'
  description: string;
  warning: string; // '' = none
  setup: string; // hardware to fit before the run (jumpers, terminator); '' = none
  machine_safe: boolean; // may be run with the card fitted in a real machine
  unbounded: boolean; // loops until stopped; takes a seconds bound
  default_seconds: number; // suggested bound, 0 = self-bounded
}

export interface SelftestRun {
  test: string;
  started_at: number;
  hint: string; // a likely cause the test named for itself; '' = it named none
}

export interface SelftestResult {
  test: string;
  verdict: string; // 'passed' | 'failed' | 'error' | 'aborted'
  hint: string; // a likely cause the test named for itself; '' = it named none
  exit_code: number; // -1: ended by a signal
  started_at: number;
  ended_at: number;
}

export interface SelftestState {
  running: SelftestRun | null;
  last: SelftestResult | null;
}

// ---- hardware / bus state ----
export interface HwState {
  // The backplane's power signals as the board read them, or null for a bus
  // nothing is reading — see 10.05_web/2_src/webbuspower.hpp. They are the
  // state of the bus, not of the emulated machine's power switch.
  dcok: boolean | null;
  pok: boolean | null;
  // Whether the emulation is installed on the bus (dc_on/dc_off). null until a
  // state frame says; everything that gates on it asks `!== false`, so an
  // unknown reads as installed the way the old default did.
  powered: boolean | null;
  leds: boolean[];
  dip: boolean[];
}
export interface BusState {
  halted: boolean;
  init: boolean;
}

// ---- what the machine is doing, from the `metrics` event ----

// What one count is, which is what decides how a rate is rendered: bytes become
// KB/s, everything else is counted per second. See 10.01_base/2_src/arm/metric.hpp.
export type MetricUnit = 'count' | 'byte' | 'instruction';

// One measured rate of one device. `pct` and `reference` are present only where
// there is something to compare against, which today is an emulated processor
// and nothing else.
export interface DevMetric {
  name: string; // the key an API caller matches on
  unit: MetricUnit;
  label: string; // a couple of words for the head of the row
  rate: number; // units a second, over the last sampling interval
  pct?: number; // percentage of what the original machine ran at
  reference?: number;
}

// One device's report. `kind` is the device's category — "cpu", "disk",
// "network" — which is what groups the rows and picks how they read.
export interface DevMetrics {
  dev: string;
  type: string;
  kind: string;
  metrics: DevMetric[];
}

// The panel's own memory of the stream: the latest report per device, in the
// order the board sent it, and a bounded history per (device, metric) for the
// sparklines. History is the client's — the board publishes a rate and keeps
// nothing — so it starts when the page opens and is lost with it.
export interface MetricsState {
  devs: DevMetrics[];
  // key is "<dev>/<metric>"; oldest first, newest last
  history: Record<string, number[]>;
  // whether any metrics frame has arrived, so the panel can tell "nothing to
  // report" from "not heard from the board yet"
  seen: boolean;
}

// What carries the machine's console: the ttyS2 bridge to a physical console
// SLU, a Web Serial port in the browser, or the emulated DL11 at 777560.
export type ConsoleSource = 'ttys2' | 'webserial' | 'dl11' | 'vax';

// ---- the debug panel ----

// Where a processor's state was read from: a core of the board's own, the bus,
// or nowhere. See 10.05_web/docs/api.md on GET /api/debug/cpu.
export type DebugSource = 'emulated' | 'bus' | 'none';

export interface DebugRegister {
  name: string;
  value: number;
}

// One address the bus probe tried. `value` is null when the cycle timed out,
// and `name` is there only for the points whose meaning is the same on every
// model that answers them.
export interface DebugProbePoint {
  address: number;
  name?: string;
  // what the address means on a PDP-11, where it means anything
  info?: string;
  value: number | null;
}

export interface DebugPsw {
  value: number;
  priority: number;
  t: boolean;
  n: boolean;
  z: boolean;
  v: boolean;
  c: boolean;
  has_modes: boolean;
  mode?: string;
  previous_mode?: string;
}

// The page registers of one processor mode: eight page address registers and
// the eight page descriptors beside them, page 0 first. Two arrays rather than
// eight pairs, because that is what they are — separate registers at separate
// addresses, which happen to be read together.
export interface DebugMmuPages {
  par: number[];
  pdr: number[];
}

// One disassembled instruction. `words` is as long as the instruction is: on a
// PDP-11 an instruction is only as long as its operands make it.
export interface DebugInstruction {
  address: number;
  words: number[];
  mnemonic: string;
  operands: string;
  known: boolean; // an instruction on some PDP-11
  available: boolean; // ... and on this model
  truncated: boolean; // a word of it could not be read
  comment?: string;
  // addresses the operands name which mean something: device registers, the
  // processor and memory management registers, trap and interrupt vectors
  known_addresses?: { address: number; info: string }[];
}

export interface DebugListing {
  address: number;
  next: number; // where a following listing continues
  model: string;
  options: string;
  instructions: DebugInstruction[];
  complete: boolean; // false: memory stopped answering before the count was met
  reason?: string;
}

export interface DebugCpu {
  source: DebugSource;
  available: boolean;
  reason?: string;
  device?: string;
  model?: string;
  run_state?: 'halted' | 'running' | 'waiting';
  registers?: DebugRegister[];
  stackpointers?: DebugRegister[];
  psw?: DebugPsw;
  ir?: number;
  bus_addr?: number;
  bus_data?: number;
  cycle_count?: number;
  mmu?: {
    enabled: boolean;
    mmr0: number;
    mmr1: number;
    mmr2: number;
    // the eight page registers of each mode, page 0 first. Absent from a
    // processor read over the bus: they are internal to the CPU.
    kernel?: DebugMmuPages;
    user?: DebugMmuPages;
  };
  probe?: DebugProbePoint[];
  powered: boolean;
  halted: boolean;
  addr_width: number;
}
