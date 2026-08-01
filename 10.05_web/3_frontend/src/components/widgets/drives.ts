// The drive widgets: a bezel of switch caps over a foot carrying the medium.
import { html } from '../../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../../types';
import { liveSetParam } from '../../api';
import { alertModal } from '../../lib/modals';
import { ImageField } from '../common';
import { statusParam } from '../../lib/devmodel';
import { Cap, PanelWidget, ReadyCap, paramVal } from './base';

// The drive foot: the shared image picker, which is the drive's image-swap
// control. The medium is always changeable — a drive that models a spin-up
// brings the pack to rest as part of the change — so the foot carries no tag
// saying whether it is.
function DiskFoot({ d }: { d: LiveDev }) {
  const img = d.img || paramVal(d, 'image');
  // A medium puts the drive in the machine, which the machine can refuse; the
  // operator is told so rather than left with a drive that took no pack.
  const pick = async (name: string) => {
    const res = await liveSetParam(
      d.name,
      'image',
      name,
      name ? 'image attached' : 'image detached'
    );
    if (!res.ok)
      await alertModal(
        name ? 'Medium not attached' : 'Medium not detached',
        res.data.error || 'The machine refused the change.'
      );
  };
  return html`<${ImageField} drive=${d.name} image=${img} onPick=${pick} />`;
}

// What every drive shares: a unit number on its READY cap, the medium it holds,
// and a foot where the operator swaps that medium.
export abstract class DriveWidget extends PanelWidget {
  protected unit(): string {
    return this.param('unit') || this.d.name.replace(/\D/g, '');
  }
  protected loaded(): boolean {
    return !!(this.d.img || this.param('image'));
  }
  protected foot(): ComponentChildren {
    return html`<${DiskFoot} d=${this.d} />`;
  }
}

// The RL01/RL02 front bezel: LOAD, READY, FAULT and WRITE-PROT. The lamps come
// from statusParams; LOAD and WRITE-PROT are operator buttons that toggle the
// drive's run/stop and write-protect configuration switches.
export class RlWidget extends DriveWidget {
  protected caps(): ComponentChildren {
    const rsToggle = () => {
      const on = this.param('runstopbutton') === '1';
      liveSetParam(this.d.name, 'runstopbutton', on ? '0' : '1',
        on ? 'pack spins down, LOAD lights when safe' : 'loading — READY after spin-up');
    };
    const wpToggle = () => {
      const on = this.param('writeprotectbutton') === '1';
      liveSetParam(this.d.name, 'writeprotectbutton', on ? '0' : '1',
        on ? 'write protection released' : 'write-protected');
    };
    return html`${html`<${Cap} cls="cap-yellow" lit=${this.lit(this.lamp('loadlamp'))} onClick=${rsToggle}>LOAD</${Cap}>`}
      ${html`<${ReadyCap} unit=${this.unit()} lit=${this.lit(this.lamp('readylamp'))} />`}
      ${html`<${Cap} cls="cap-red" lit=${this.lit(this.lamp('faultlamp'))}>FAULT</${Cap}>`}
      ${html`<${Cap} cls="cap-orange" lit=${this.lit(this.lamp('writeprotectlamp'))} onClick=${wpToggle}>WRITE<br />PROT</${Cap}>`}`;
  }
}

// The RA81/MSCP drive: RUN-STOP, a momentary FAULT test, WRITE-PROT, the unit's
// READY cap, and the dual-port A/B selector. RUN-STOP follows the loaded medium
// and ACCESS the drive's activity lamp; FAULT and WRITE-PROT are the operator's
// own front-panel switches, held on the drive.
const RA81_SWITCHES = new Map<string, { wp: boolean; fault: boolean }>();
function ra81Switches(name: string): { wp: boolean; fault: boolean } {
  if (!RA81_SWITCHES.has(name)) RA81_SWITCHES.set(name, { wp: false, fault: false });
  return RA81_SWITCHES.get(name)!;
}
export class Ra81Widget extends DriveWidget {
  protected panelCls = 'span2';
  protected caps(): ComponentChildren {
    const [, force] = useState(0);
    const sw = ra81Switches(this.d.name);
    const mounted = this.loaded();
    const active = this.lamp('accesslamp');
    const toggle = (k: 'wp' | 'fault') => {
      sw[k] = !sw[k];
      force((x) => x + 1);
    };
    useEffect(() => {
      // FAULT is momentary: it lights while held and releases on mouse-up
      const up = () => {
        if (sw.fault) {
          sw.fault = false;
          force((x) => x + 1);
        }
      };
      document.addEventListener('mouseup', up);
      return () => document.removeEventListener('mouseup', up);
    }, []);
    return html`${html`<${Cap} cls="cap-yellow" lit=${this.lit(mounted)}>RUN<br />STOP</${Cap}>`}
      <button type="button" class=${'lampbtn cap-red' + (this.lit(sw.fault) ? ' lit' : '')}
        onMouseDown=${() => toggle('fault')}>FAULT</button>
      ${html`<${ReadyCap} unit=${this.unit()} lit=${this.lit(mounted && !active)} wide=${true} />`}
      <button type="button" class=${'lampbtn cap-yellow' + (this.lit(sw.wp) ? ' lit' : '')}
        onClick=${() => toggle('wp')}>WRITE<br />PROT</button>
      ${html`<${Cap} cls="cap-white legend-lg" lit=${this.lit(true)}>A</${Cap}>`}
      ${html`<${Cap} cls="cap-white legend-lg" lit=${false}>B</${Cap}>`}`;
  }
}

// Every other disk (RK05, RS11): a READY cap with the unit number and an ACCESS
// lamp that follows the drive's activity.
export class DiskWidget extends DriveWidget {
  protected panelCls = 'span2';
  protected caps(): ComponentChildren {
    const active = this.lamp('accesslamp');
    return html`${html`<${ReadyCap} unit=${this.unit()} lit=${this.lit(this.d.enabled && !active)} />`}
      ${html`<${Cap} cls="cap-yellow" lit=${this.lit(active)}>ACCESS</${Cap}>`}`;
  }
}

// The RX01/RX02 floppy and the TK50/TK70 cartridge drive carry the same bezel: a
// READY cap with the unit number, a USE lamp that follows the drive's ACCESS
// activity, and a WRITE PROT lamp for a write-protected medium. The mounted
// image and its removable tag sit on the foot, where the operator swaps media.
export class RemovableDriveWidget extends DriveWidget {
  protected caps(): ComponentChildren {
    const active = this.lamp('accesslamp');
    return html`${html`<${ReadyCap} unit=${this.unit()} lit=${this.lit(this.loaded() && !active)} />`}
      ${html`<${Cap} cls="cap-yellow" lit=${this.lit(active)}>USE</${Cap}>`}
      ${html`<${Cap} cls="cap-orange" lit=${this.lit(this.lamp('writeprotectlamp'))}>WRITE<br />PROT</${Cap}>`}`;
  }
}

// The RX01/RX02 floppy: the removable bezel over a foot that also carries the
// sector order of the medium. A floppy image comes either as the surface was
// written or as the volume's blocks in order, and the drive reads which when the
// medium goes in. The control names what it settled on and lets the operator
// call the medium physical or logical outright.
export class FloppyDriveWidget extends RemovableDriveWidget {
  protected foot(): ComponentChildren {
    const chosen = this.param('layout') || 'auto';
    const inUse = statusParam(this.d, 'layoutinuse')?.v || 'physical';
    const pick = async (v: string) => {
      const res = await liveSetParam(this.d.name, 'layout', v, 'sector order ' + v);
      if (!res.ok)
        await alertModal(
          'Sector order not set',
          res.data.error || 'The drive refused the change.'
        );
    };
    return html`${super.foot()}
      <div class="layoutrow">
        <span class="layoutlegend">SECTOR ORDER</span>
        <select class="layoutsel mono" value=${chosen}
          onChange=${(e: Event) => pick((e.target as HTMLSelectElement).value)}>
          <option value="auto">auto — ${inUse}</option>
          <option value="physical">physical</option>
          <option value="logical">logical</option>
        </select>
      </div>`;
  }
}
