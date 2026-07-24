// Builds and mutates the live device model from /api/devices and /ws/events.
import { store } from '../store';
import { octalStr } from './util';
import type { ApiDevice, ApiParam, LiveDev, LiveParam } from '../types';

function liveParam(p: ApiParam): LiveParam {
  const out: LiveParam = {
    n: p.name,
    s: p.shortname,
    ro: p.readonly,
    i: p.info || '',
    u: p.unit || '',
    t: 'str',
    v: '',
  };
  if (p.type === 'unsigned' || p.type === 'unsigned64') {
    if (p.base === 8) {
      out.t = 'oct';
      out.v = octalStr(Number(p.value), p.bitwidth);
      out.bw = p.bitwidth;
    } else {
      out.t = 'uint';
      out.v = String(p.value);
    }
  } else if (p.type === 'double') {
    out.t = 'dbl';
    out.v = String(p.value);
  } else if (p.type === 'bool') {
    out.t = 'str';
    out.v = p.value ? '1' : '0';
  } else if (p.type === 'enum') {
    out.t = 'enum';
    out.v = p.value == null ? '' : String(p.value);
    out.opts = p.values || p.options || [];
  } else {
    out.t = 'str';
    out.v = p.value == null ? '' : String(p.value);
  }
  return out;
}

export function liveModel(devs: ApiDevice[]): LiveDev[] {
  const hidden = ['name', 'type', 'enabled'];
  const map = new Map<string, LiveDev>(
    devs.map((d) => [
      d.name,
      {
        name: d.name,
        type: d.type,
        category: d.category,
        enabled: d.enabled,
        removable: d.removable,
        locked: d.locked,
        info: '',
        params: d.params.filter((p) => !hidden.includes(p.name)).map(liveParam),
        drives: [] as LiveDev[],
        img: String((d.params.find((p) => p.name === 'image') || { value: '' }).value || ''),
      },
    ])
  );
  const roots: LiveDev[] = [];
  devs.forEach((d) => {
    const e = map.get(d.name)!;
    if (d.parent && map.has(d.parent)) map.get(d.parent)!.drives.push(e);
    else roots.push(e);
  });
  return roots;
}

export function walkDevs(fn: (d: LiveDev) => void, list: LiveDev[] = store.devmodel): void {
  list.forEach((d) => {
    fn(d);
    walkDevs(fn, d.drives || []);
  });
}

export function enabledDevices(): LiveDev[] {
  const out: LiveDev[] = [];
  walkDevs((d) => {
    if (d.enabled) out.push(d);
  });
  return out;
}

export function devEnabled(name: string): boolean {
  let f = false;
  walkDevs((d) => {
    if (d.name === name && d.enabled) f = true;
  });
  return f;
}

// ---- live parameter values from /ws/events, applied and replayed ----
const EVENT_VALUES = new Map<string, unknown>();

export function applyParamValue(dev: string, name: string, value: unknown): void {
  walkDevs((d) => {
    if (d.name === dev) {
      if (name === 'enabled') d.enabled = !!value;
      if (name === 'image') d.img = String(value || '');
      const p = d.params.find((q) => q.n === name);
      if (p) {
        if (p.t === 'oct') p.v = octalStr(Number(value), p.bw);
        else if (typeof value === 'boolean') p.v = value ? '1' : '0';
        else p.v = value == null ? '' : String(value);
      }
    }
  });
}

export function patchParam(dev: string, name: string, value: unknown): void {
  EVENT_VALUES.set(dev + '\0' + name, value);
  applyParamValue(dev, name, value);
}

export function replayEventValues(): void {
  EVENT_VALUES.forEach((value, key) => {
    const sep = key.indexOf('\0');
    applyParamValue(key.slice(0, sep), key.slice(sep + 1), value);
  });
}
