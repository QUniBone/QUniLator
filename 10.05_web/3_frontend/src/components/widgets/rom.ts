// The bootstrap PROM card, laid out like the memory card it sits beside: the
// ACCESS lamp in the black bezel, and one fact line for what the sockets hold.
// The addresses the module answers at are its own and never move, so the card
// says what is programmed into it and leaves the decode to the device page.
import { html } from '../../html';
import type { ComponentChildren } from 'preact';
import { statusParam } from '../../lib/devmodel';
import { Cap, PanelWidget } from './base';

export class RomWidget extends PanelWidget {
  protected panelCls = 'memcard';

  // The PRU answers the module on its own, so whether the machine is reading it
  // is the one thing no readout otherwise says.
  protected caps(): ComponentChildren {
    return html`<${Cap} cls="cap-yellow" lit=${this.lit(this.lamp('accesslamp'))}>ACCESS</${Cap}>`;
  }

  protected foot(): ComponentChildren {
    const st = (n: string): string => {
      const p = statusParam(this.d, n);
      return p ? p.v : '';
    };
    const size = st('arraysize');
    return html`<div class="rl-info">
      <div>${st('contents') || 'empty'}${size ? ' · ' + size : ''}</div>
    </div>`;
  }
}
