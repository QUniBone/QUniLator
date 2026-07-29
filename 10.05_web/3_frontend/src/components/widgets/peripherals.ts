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

// The DZV11 4-line mux, laid out like a modem panel: a black lamp window with
// one row of signal lamps per line and a legend of the signal names beneath, in
// the control-panel legend font. Only the signals the DZV11 carries appear:
// RX/TX traffic, DTR (driven by the guest), CD (a connected TCP client), and RI
// (a modem-status ring bit; unasserted, as the TCP transport has no ring). The
// DZV11 has no RTS/CTS/DSR silicon, so those signals are absent.
const DZ_LINES = 4;
const DZ_SIGNALS: { key: string; label: string; live: boolean }[] = [
  { key: 'rx', label: 'RX', live: true },
  { key: 'tx', label: 'TX', live: true },
  { key: 'dtr', label: 'DTR', live: true },
  { key: 'cd', label: 'CD', live: true },
  { key: 'ri', label: 'RI', live: false },
];
export class DzWidget extends DeviceWidget {
  render(): ComponentChildren {
    const rows = [];
    for (let ln = 0; ln < DZ_LINES; ln++) {
      const cells = DZ_SIGNALS.map((s) => {
        const on = this.lit(s.live && this.lamp(s.key + ln + 'lamp'));
        return html`<span class="dz-cell"><${Led} on=${on} title=${s.label + ' line ' + ln} /></span>`;
      });
      rows.push(html`<div class="dz-row"><span class="dz-port">${ln}</span>${cells}</div>`);
    }
    const legend = html`<div class="dz-legend"><span class="dz-port"></span>${DZ_SIGNALS.map(
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
        <div class="vcb01-hint">click to focus — keyboard & mouse drive the board</div></div></div>`;
  }
}
