// /ws/events: committed parameter changes, log lines and hardware state.
import { store, setStore, emit, emitSoon } from '../store';
import { refreshDevices, refreshSettings } from '../api';
import { patchParam, patchStatus } from './devmodel';
import { clearConsole, updateConsoleSource } from './terminals';
import { wsURL } from './util';
import { syncVersion } from './version';
import type { CatalogJob, DevMetrics, LogLevelName, LogLine, UpdateStatus } from '../types';

const LOG_LEVELS: Record<number, LogLevelName> = {
  1: 'FATAL',
  2: 'ERROR',
  3: 'WARNING',
  4: 'INFO',
  5: 'DEBUG',
};

// How many 1 Hz samples the performance panel keeps per metric: a minute, which
// is long enough to show a burst against the idle around it and short enough to
// draw legibly in the width of a card.
export const METRIC_HISTORY = 60;

export let eventsWs: WebSocket | null = null;

export function initEvents(): void {
  eventsWs = new WebSocket(wsURL('/ws/events'));
  eventsWs.onopen = () => {
    setStore({ connected: true });
    // A reconnect is where a replaced service is noticed: the socket dropping
    // and coming back is exactly what a restart looks like from here, so this
    // is where the page checks whether it is still talking to its own version.
    syncVersion().catch(() => {});
  };
  // One frame carries a whole poll cycle's events. Each is applied to the
  // store, and the re-render is asked for once at the end: a running machine
  // moves a dozen status parameters per cycle, and a repaint per parameter is
  // a dozen renders where one will do.
  eventsWs.onmessage = (e) => {
    let ev: any;
    try {
      ev = JSON.parse(e.data);
    } catch {
      return;
    }
    if (ev.t === 'batch') {
      let soon = false,
        now = false;
      for (const item of ev.events || []) {
        if (applyEvent(item) === 'soon') soon = true;
        else now = true;
      }
      if (now) emit();
      else if (soon) emitSoon();
      return;
    }
    const how = applyEvent(ev);
    if (how === 'soon') emitSoon();
    else emit();
  };

  // Apply one event to the store. Returns how urgently the screen should
  // follow: a lamp storm is damped, everything else repaints at once.
  function applyEvent(ev: any): 'soon' | 'now' {
    if (ev.t === 'param') {
      patchParam(ev.dev, ev.param, ev.value);
      // a device coming or going can be the one that carries the console — a
      // VAX holds its console inside the processor — so re-point the terminal
      // on the switch rather than waiting for the next full device reload
      if (ev.param === 'enabled') updateConsoleSource();
      return /lamp$/.test(ev.param) ? 'soon' : 'now';
    } else if (ev.t === 'status') {
      // the drive's verbal state, so the chip follows a drive the machine moves
      // on its own. It turns on the access lamp among others, so it flips as
      // fast as one during a transfer and is damped the same way.
      patchStatus(ev.dev, ev.status);
      return 'soon';
    } else if (ev.t === 'log') {
      // the journal assigns the id and server timestamp; append only when this
      // line is newer than the last held, so a live frame never duplicates one
      // already loaded from the journal page
      const line: LogLine = {
        id: ev.id || 0,
        t: ev.time || new Date().toTimeString().slice(0, 8),
        lvl: LOG_LEVELS[ev.level] || 'INFO',
        src: ev.label,
        msg: String(ev.text).replace(/^\[[^\]]*\]\s*/, ''),
      };
      const log = store.log;
      if (!log.length || line.id > log[log.length - 1].id) {
        log.push(line);
        // bound in-memory growth from the live stream; older entries page back
        // in from the journal on demand
        if (log.length > 5000) {
          log.shift();
          store.logMore = true;
        }
      }
      return 'now';
    } else if (ev.t === 'state') {
      const hw = store.hw,
        bus = store.bus;
      // state frames may be partial (e.g. {"t":"state","powered":false}); merge
      // each field only when present so the last-known value holds otherwise
      if ('halt' in ev) bus.halted = ev.halt;
      if ('init' in ev) bus.init = ev.init;
      if ('dcok' in ev) hw.dcok = ev.dcok;
      if ('pok' in ev) hw.pok = ev.pok;
      if ('powered' in ev) {
        // switched on: the machine has printed nothing yet
        if (ev.powered && hw.powered === false) clearConsole();
        // The board carries its machine across the power switch, and a card out
        // of the emulation is still in it. Which cards those are, and what is in
        // their drives, comes from the device set as the board reports it.
        if (ev.powered !== hw.powered) refreshDevices().catch(() => {});
        hw.powered = ev.powered;
      }
      if (ev.leds) ev.leds.forEach((v: boolean, i: number) => (hw.leds[i] = v));
      if (ev.switches) ev.switches.forEach((v: boolean, i: number) => (hw.dip[i] = v));
      if ('notice' in ev) store.notice = typeof ev.notice === 'string' ? ev.notice : '';
      if ('held_by' in ev) {
        const held = typeof ev.held_by === 'string' ? ev.held_by : '';
        // Whatever held the board was free to change the machine while it had
        // it — a power-up rebuilds every card — so the device set is read again
        // once it lets go.
        if (held === '' && store.heldBy !== '') refreshDevices().catch(() => {});
        store.heldBy = held;
      }
      return 'now';
    } else if (ev.t === 'metrics') {
      // The whole set, once a second: the devices reporting, each with the rate
      // it measured over the last interval. It replaces what was held rather
      // than merging, so a device that has stopped reporting — switched off,
      // taken out of the configuration — simply leaves.
      const devs: DevMetrics[] = ev.devs || [];
      const history = store.metrics.history;
      const live = new Set<string>();
      for (const d of devs)
        for (const m of d.metrics || []) {
          const key = d.dev + '/' + m.name;
          live.add(key);
          const h = history[key] || (history[key] = []);
          h.push(m.rate);
          // a minute of samples, which is what the sparkline draws
          if (h.length > METRIC_HISTORY) h.shift();
        }
      // A device that stopped reporting keeps nothing: coming back, its old
      // trace beside a fresh one would draw a minute that never happened.
      for (const key of Object.keys(history)) if (!live.has(key)) delete history[key];
      store.metrics.devs = devs;
      store.metrics.seen = true;
      // A rate that moves once a second is not what a page is repainting for;
      // let it ride with whatever else the cycle brought.
      return 'soon';
    } else if (ev.t === 'update') {
      // the whole update status, as GET /api/update answers it; the frame is
      // broadcast on every change and opens each connection
      const { t, ...status } = ev;
      void t;
      store.update = status as UpdateStatus;
      return 'now';
    } else if (ev.t === 'catalog') {
      // the catalogue job's whole status; broadcast on every change and sent
      // on connect, so a page opened mid-download shows the bar at once
      const { t, ...job } = ev;
      void t;
      store.catalogJob = job as CatalogJob;
      return 'now';
    } else if (ev.t === 'selftest') {
      // a hardware self-test run started or ended; broadcast on each change
      // and sent on every connect
      store.selftest = { running: ev.running || null, last: ev.last || null };
      return 'now';
    } else if (ev.t === 'settings') {
      // A machine setting moved, wherever it was moved from. Reread them; that
      // re-points the console when the setting names a different port for it,
      // so a change made in another browser or over the API does not leave this
      // page holding a terminal that has gone silent.
      refreshSettings().catch(() => {});
      return 'now';
    } else if (ev.t === 'config') {
      // keep the configuration master list's current mark and the modified
      // badge live between full reloads
      if ('current' in ev) store.configCurrent = ev.current || '';
      if ('modified' in ev) store.configModified = ev.modified;
    }
    return 'now';
  }
  eventsWs.onclose = () => {
    setStore({ connected: false });
    setTimeout(initEvents, 2000);
  };
}
