// Builds and mutates the live device model from /api/devices and /ws/events.
import { store } from '../store';
import { hexStr, octalStr } from './util';
import type { ApiDevice, ApiParam, DiskStatus, LiveDev, LiveParam } from '../types';

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
  if (p.content) out.c = p.content;
  if (p.type === 'unsigned' || p.type === 'unsigned64') {
    if (p.base === 8) {
      out.t = 'oct';
      out.v = octalStr(Number(p.value), p.bitwidth);
      out.bw = p.bitwidth;
    } else if (p.base === 16) {
      out.t = 'hex';
      out.v = hexStr(Number(p.value), p.bitwidth);
      out.bw = p.bitwidth;
    } else {
      out.t = 'uint';
      out.v = String(p.value);
    }
  } else if (p.type === 'double') {
    out.t = 'dbl';
    out.v = String(p.value);
  } else if (p.type === 'bool') {
    out.t = 'bool';
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
        label: d.label,
        category: d.category,
        enabled: d.enabled,
        removable: d.removable,
        locked: d.locked,
        status: d.status,
        info: '',
        params: d.params.filter((p) => !hidden.includes(p.name)).map(liveParam),
        // Machine-driven running state; empty on the pre-split backend, which
        // is degraded gracefully rather than treated as a fault.
        statusParams: (d.statusparams || []).map(liveParam),
        drives: [] as LiveDev[],
        // the medium is the parameter the drive declares as one, not one
        // recognised by its name
        img: String((d.params.find((p) => p.content === 'image') || { value: '' }).value || ''),
        addressOptions: d.address_options,
        vectorOptions: d.vector_options,
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

// Look up a machine-driven status parameter (a lamp, LED, drive-state value or
// counter) by name; undefined when the device does not carry it.
export function statusParam(d: LiveDev, name: string): LiveParam | undefined {
  return (d.statusParams || []).find((p) => p.n === name);
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

// The friendly label of a device from the live registry, keyed by handle;
// falls back to the handle for a device the registry does not carry.
export function deviceLabel(name: string): string {
  let out = '';
  walkDevs((d) => {
    if (d.name === name) out = d.label || '';
  });
  return out || name;
}

// The live registry as a flat list (controllers and their drives), for the
// stored-config editor to enumerate the editable device set.
export function flatDevices(): LiveDev[] {
  const out: LiveDev[] = [];
  walkDevs((d) => out.push(d));
  return out;
}

// The device tree kept down to those the predicate accepts. A drive rides with
// its controller: a controller the predicate rejects takes its drives out of the
// list with it, whatever the predicate would have said about them on their own.
export function keptDevices(keep: (d: LiveDev) => boolean): LiveDev[] {
  const out: LiveDev[] = [];
  const take = (d: LiveDev): void => {
    if (!keep(d)) return;
    out.push(d);
    (d.drives || []).forEach(take);
  };
  store.devmodel.forEach(take);
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
      // A committed value can land on either collection: configuration values in
      // params, machine-driven lamp/state/counter values in statusParams.
      const p = d.params.find((q) => q.n === name) || statusParam(d, name);
      // the medium the drive declares, whatever it named the parameter
      if (p && p.c === 'image') d.img = String(value || '');
      if (p) {
        if (p.t === 'oct') p.v = octalStr(Number(value), p.bw);
        else if (p.t === 'hex') p.v = hexStr(Number(value), p.bw);
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

// The drive's verbal status, as the event stream reports it. The backend
// derives it from the drive's signals and is the single source of it, so the
// page holds what it was told rather than reading the chip off the parameters
// beneath. It needs no replay onto a rebuilt model: the GET the rebuild is made
// from carries the status as it stands, and the stream carries it on from there.
export function patchStatus(dev: string, status: string): void {
  walkDevs((d) => {
    if (d.name === dev) d.status = status as DiskStatus;
  });
}

// Restore the newest machine-driven values onto a freshly rebuilt model. Only
// status values are replayed — lamps, drive state and counters, which change
// faster than /api/devices is refetched. Configuration values (enabled, image
// and settable parameters) come authoritatively from the GET, so a stale cached
// event must never be replayed over them: doing so snaps a just-toggled device
// back to its previous state on the refresh that follows the toggle.
export function replayEventValues(): void {
  EVENT_VALUES.forEach((value, key) => {
    const sep = key.indexOf('\0');
    const dev = key.slice(0, sep),
      name = key.slice(sep + 1);
    let isStatus = false;
    walkDevs((d) => {
      if (d.name === dev && statusParam(d, name)) isStatus = true;
    });
    if (isStatus) applyParamValue(dev, name, value);
  });
}
