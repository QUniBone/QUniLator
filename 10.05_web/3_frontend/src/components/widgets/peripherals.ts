// The interface widgets: the network card, the serial mux and the video display.
import { html } from '../../html';
import { useEffect, useRef } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { Led } from '../common';
import {
  vcb01Connect,
  vcb01Blit,
  vcb01WireKeyboard,
  vcb01WireMouse,
  vcb01Disconnect,
} from '../../lib/vcb01';
import { Cap, DeviceWidget, PanelWidget } from './base';

// An Ethernet interface: a LINK lamp for the installed card and an ACT lamp
// that follows its traffic, over the host interface and address it carries.
export class NetworkWidget extends PanelWidget {
  protected caps(): ComponentChildren {
    return html`${html`<${Cap} cls="cap-white" lit=${this.lit(this.d.enabled)}>LINK</${Cap}>`}
      ${html`<${Cap} cls="cap-yellow" lit=${this.lit(this.lamp('activitylamp'))}>ACT</${Cap}>`}`;
  }
  protected foot(): ComponentChildren {
    return html`<div class="rl-info">${this.param('interface') || '—'} · ${this.param('mac') || '—'}</div>`;
  }
}

// A serial mux, laid out like a modem panel: a black lamp window with one row of
// signal lamps per line and a legend of the signal names beneath, in the
// control-panel legend font. A mux shows the signals its silicon actually
// carries, so each type declares its own set and the panel draws that.
//
// A signal marked dead has no lamp parameter behind it: the board brings the
// line out but nothing in this machine ever asserts it, so it is drawn dark
// rather than left off the panel, which is what the real one looks like.
interface MuxSignal {
  key: string;
  label: string;
  live: boolean;
}
abstract class MuxWidget extends DeviceWidget {
  protected abstract signals: MuxSignal[];

  // How many lines the board has, counted from the lamps it publishes: the same
  // register model serves a four-line and an eight-line mux, so the panel takes
  // the count from the device rather than from the type.
  protected get lines(): number {
    let n = 0;
    for (const p of this.d.statusParams || []) {
      const m = /^rx(\d+)lamp$/.exec(p.n);
      if (m) n = Math.max(n, Number(m[1]) + 1);
    }
    return n;
  }

  render(): ComponentChildren {
    const cols = '20px repeat(' + this.signals.length + ', 32px)';
    const rows = [];
    for (let ln = 0; ln < this.lines; ln++) {
      const cells = this.signals.map((s) => {
        const on = this.lit(s.live && this.lamp(s.key + ln + 'lamp'));
        return html`<span class="dz-cell"><${Led} on=${on} title=${s.label + ' line ' + ln} /></span>`;
      });
      rows.push(
        html`<div class="dz-row" style=${'grid-template-columns:' + cols}>
          <span class="dz-port">${ln}</span>${cells}</div>`
      );
    }
    const legend = html`<div class="dz-legend" style=${'grid-template-columns:' + cols}>
      <span class="dz-port"></span>${this.signals.map(
        (s) => html`<span class="dz-cell">${s.label}</span>`
      )}</div>`;
    return html`<div class="card diskcard dzcard">
      ${this.head()}
      <div class="card-body dzbody">
        <div class="dz-panel">${rows}</div>
        ${legend}
      </div></div>`;
  }
}

// The signals read in the pairs they work in: the two data directions, then the
// two handshakes, then the two ready lines, then the two the far end raises. A
// board that lacks one of a pair simply omits it and the order holds.

// The DZ11 family's lines carry RX/TX traffic, DTR (driven by the guest), and
// the two the transport's modem raises: RI while a call is ringing and CD while
// a client holds the line. The board has no RTS/CTS/DSR silicon, so those
// signals are absent.
export class DzWidget extends MuxWidget {
  protected signals: MuxSignal[] = [
    { key: 'rx', label: 'RX', live: true },
    { key: 'tx', label: 'TX', live: true },
    { key: 'dtr', label: 'DTR', live: true },
    { key: 'ri', label: 'RI', live: true },
    { key: 'cd', label: 'CD', live: true },
  ];
}

// The DHV11 carries full modem control on all eight lines: DTR and RTS out of
// LNCTRL, and CD/DSR/CTS reported back through STAT, which a connected client
// asserts together. RI is brought out and never rung.
export class DhWidget extends MuxWidget {
  protected signals: MuxSignal[] = [
    { key: 'rx', label: 'RX', live: true },
    { key: 'tx', label: 'TX', live: true },
    { key: 'rts', label: 'RTS', live: true },
    { key: 'cts', label: 'CTS', live: true },
    { key: 'dsr', label: 'DSR', live: true },
    { key: 'dtr', label: 'DTR', live: true },
    { key: 'ri', label: 'RI', live: false },
    { key: 'cd', label: 'CD', live: true },
  ];
}

// The VCB01/QVSS bitmap display, drawn from the frame-buffer stream the board
// sends. The socket lives as long as the card is on the dashboard: the widget
// opens it on mount and closes it when the card goes, and the stream's own
// reconnect stops once the canvas has left the page.
export class Vcb01Widget extends DeviceWidget {
  render(): ComponentChildren {
    const cv = useRef<HTMLCanvasElement | null>(null);
    useEffect(() => {
      vcb01Connect();
      vcb01Blit();
      vcb01WireKeyboard();
      if (cv.current) vcb01WireMouse(cv.current);
      return () => vcb01Disconnect();
    }, []);
    return html`<div class="card diskcard vcb01-widget">
      ${this.head()}
      <div class="card-body diskface">
        <div class="vcb01-screen"><canvas id="vcb01-canvas" ref=${cv} tabindex="0"></canvas></div>
        <div class="vcb01-hint">click to focus — keyboard & mouse drive the machine</div></div></div>`;
  }
}
