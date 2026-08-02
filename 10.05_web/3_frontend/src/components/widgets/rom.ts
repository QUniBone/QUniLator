// The bootstrap PROM card: what is in the sockets and where the array reaches
// the machine. The addresses the bootstrap answers at are the module's own and
// never move, so the card names the pages the windows show rather than the
// addresses they show them at; the memory-space range is the operator's and is
// spelled out.
import { html } from '../../html';
import type { ComponentChildren } from 'preact';
import { statusParam } from '../../lib/devmodel';
import { DeviceWidget } from './base';

export class RomWidget extends DeviceWidget {
  render(): ComponentChildren {
    const st = (n: string): string => {
      const p = statusParam(this.d, n);
      return p ? p.v : '';
    };
    const windows = st('windows');
    const range = st('range');

    return html`<div class="card romcard">
      ${this.head(this.accessCap())}
      <div class="card-body rombody">
        <div class="rom-sockets">
          <span class="rom-chip"></span>
          <span class="rom-label">${st('contents') || 'empty'}</span>
          <span class="rom-size">${st('arraysize')}</span>
        </div>
        <div class="rom-rows">
          <div class="rom-row">
            <span class="rom-name">bootstrap</span>
            ${windows && windows !== 'off'
              ? html`<span class="rom-val">window 0 · page ${windows.split(' / ')[0]}</span>
                  <span class="rom-val">window 1 · page ${windows.split(' / ')[1]}</span>`
              : html`<span class="rom-off">strapped off</span>`}
          </div>
          <div class="rom-row">
            <span class="rom-name">array</span>
            ${range && range !== 'off'
              ? html`<span class="rom-addr">${range}</span>`
              : html`<span class="rom-off">not in memory space</span>`}
          </div>
        </div>
      </div></div>`;
  }
}
