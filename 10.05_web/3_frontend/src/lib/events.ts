// /ws/events: committed parameter changes, log lines and hardware state.
import { store, setStore, emit, emitSoon } from '../store';
import { patchParam } from './devmodel';
import { wsURL } from './util';
import type { LogLevelName } from '../types';

const LOG_LEVELS: Record<number, LogLevelName> = {
  1: 'FATAL',
  2: 'ERROR',
  3: 'WARNING',
  4: 'INFO',
  5: 'DEBUG',
};

export let eventsWs: WebSocket | null = null;

export function initEvents(): void {
  eventsWs = new WebSocket(wsURL('/ws/events'));
  eventsWs.onopen = () => setStore({ connected: true });
  eventsWs.onmessage = (e) => {
    let ev: any;
    try {
      ev = JSON.parse(e.data);
    } catch {
      return;
    }
    if (ev.t === 'param') {
      patchParam(ev.dev, ev.param, ev.value);
      if (/lamp$/.test(ev.param)) emitSoon();
      else emit();
    } else if (ev.t === 'log') {
      store.log.push({
        t: new Date().toTimeString().slice(0, 8),
        lvl: LOG_LEVELS[ev.level] || 'INFO',
        src: ev.label,
        msg: String(ev.text).replace(/^\[[^\]]*\]\s*/, ''),
      });
      if (store.log.length > 500) store.log.shift();
      emit();
    } else if (ev.t === 'state') {
      const hw = store.hw,
        bus = store.bus;
      // state frames may be partial (e.g. {"t":"state","powered":false}); merge
      // each field only when present so the last-known value holds otherwise
      if ('halt' in ev) bus.halted = ev.halt;
      if ('init' in ev) bus.init = ev.init;
      if ('dcok' in ev) hw.dcok = ev.dcok;
      if ('pok' in ev) hw.pok = ev.pok;
      if ('powered' in ev) hw.powered = ev.powered;
      if (ev.leds) ev.leds.forEach((v: boolean, i: number) => (hw.leds[i] = v));
      if (ev.switches) ev.switches.forEach((v: boolean, i: number) => (hw.dip[i] = v));
      emit();
    } else if (ev.t === 'config') {
      // keep the configuration master list's current/default marks and the
      // modified badge live between full reloads
      if ('current' in ev) store.configCurrent = ev.current || '';
      if ('default' in ev) store.configDefault = ev.default || '';
      if ('modified' in ev) store.configModified = ev.modified;
      emit();
    }
  };
  eventsWs.onclose = () => {
    setStore({ connected: false });
    setTimeout(initEvents, 2000);
  };
}
