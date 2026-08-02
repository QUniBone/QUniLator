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
}

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
}
export interface Settings {
  platform: string;
  address_width: number;
  external_console: ExternalConsole;
  // The board is either a peripheral of a real PDP-11 or the machine itself.
  // Only a UNIBUS build carries the emulated processors, so a QBUS board
  // reports emulated_cpu_available false and is offered no choice.
  emulated_cpu?: boolean;
  emulated_cpu_available?: boolean;
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

// ---- hardware / bus state ----
export interface HwState {
  dcok: boolean;
  pok: boolean;
  powered: boolean; // logical power flag (dc_on/dc_off); defaults on
  leds: boolean[];
  dip: boolean[];
}
export interface BusState {
  halted: boolean;
  init: boolean;
}

// What carries the machine's console: the ttyS2 bridge to a physical console
// SLU, a Web Serial port in the browser, or the emulated DL11 at 777560.
export type ConsoleSource = 'ttys2' | 'webserial' | 'dl11' | 'vax';
