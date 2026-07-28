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
export type LiveParamKind = 'oct' | 'uint' | 'dbl' | 'str' | 'enum' | 'bool';

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
// A dashboard card's placement: top-left grid cell, and whether it is hidden.
export interface WidgetPlace {
  x: number;
  y: number;
  hidden?: boolean;
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

export type TermKey = 'slu0' | 'slu1' | 'serial';
