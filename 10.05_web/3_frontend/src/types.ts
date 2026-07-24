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

export interface ApiDevice {
  name: string;
  type: string;
  label?: string;
  category?: string;
  enabled: boolean;
  parent?: string;
  removable?: boolean;
  locked?: boolean;
  params: ApiParam[];
}

// ---- derived live model ----
export type LiveParamKind = 'oct' | 'uint' | 'dbl' | 'str' | 'enum';

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
  category?: string;
  enabled: boolean;
  removable?: boolean;
  locked?: boolean;
  info: string;
  params: LiveParam[];
  drives: LiveDev[];
  img: string;
  activity?: boolean;
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
  path?: string;
  size: number;
  mtime?: string;
  attached?: string[];
  used?: ImageUse[];
}

export interface ConfigSummary {
  name: string;
  mtime?: string;
  enabled?: string[];
  default?: boolean;
  // filled in by the frontend after loading each snapshot
  snapshot?: ConfigSnapshot | null;
  loaded?: boolean;
}
export interface ConfigDeviceSnapshot {
  name: string;
  enabled: boolean;
  params?: Record<string, string>;
}
export interface ConfigSnapshot {
  devices: ConfigDeviceSnapshot[];
}

// ---- log lines (assembled from /ws/events) ----
export type LogLevelName = 'FATAL' | 'ERROR' | 'WARNING' | 'INFO' | 'DEBUG';
export interface LogLine {
  t: string;
  lvl: LogLevelName;
  src: string;
  msg: string;
}

// ---- hardware / bus state ----
export interface HwState {
  dcok: boolean;
  pok: boolean;
  leds: boolean[];
  dip: boolean[];
}
export interface BusState {
  halted: boolean;
  init: boolean;
}

export type TermKey = 'slu0' | 'slu1' | 'serial';
