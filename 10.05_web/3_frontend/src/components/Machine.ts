import { html } from '../html';
import { useEffect } from 'preact/hooks';
import { useStore } from '../store';
import { putSettings, refreshSettings } from '../api';
import { serialConnect, serialDisconnect, serialConnected } from '../lib/terminals';

export function MachinePage() {
  const s = useStore();
  useEffect(() => {
    refreshSettings().catch(() => {});
  }, []);
  const ec = s.settings.external_console || {};
  return html`<section class="page active" data-page="machine">
    <p class="lede">Machine-wide hardware settings — global to the whole machine, not tied to any one emulated device.</p>
    <div class="card" style="margin-bottom:14px; max-width:560px">
      <div class="card-head"><h3>Bus</h3></div>
      <div class="card-body"><div class="set-grid">
        <div class="set-name">Platform</div>
        <div class="set-val"><span class="pill mono">${s.settings.platform || '—'}</span></div>
        <div class="set-info">Bus type, fixed at build time.</div>
        <div class="set-name">Address width</div>
        <div class="set-val"><select class="mono" value=${String(s.settings.address_width)}
          onChange=${(e: Event) =>
            putSettings(
              { address_width: parseInt((e.target as HTMLSelectElement).value, 10) },
              'address width set'
            )}>
          <option value="16">16-bit</option><option value="18">18-bit</option><option value="22">22-bit</option></select></div>
        <div class="set-info">CPU address width. Changing it re-bases the I/O page, so it applies only while the bus is halted.</div>
        ${
          s.settings.emulated_cpu_available
            ? html`<div class="set-name">Processor</div>
              <div class="set-val">
                ${(
                  [
                    [false, 'Real, on the bus'],
                    [true, 'Emulated'],
                  ] as const
                ).map(
                  ([v, lbl]) =>
                    html`<label class="radio"><input type="radio" name="ecpu" checked=${!!s.settings.emulated_cpu === v}
                    onChange=${() => putSettings({ emulated_cpu: v }, 'processor set')} /> ${lbl}</label>`
                )}
              </div>
              <div class="set-info">Who arbitrates the bus. Emulated adds the KA11, KD11 and VAX processors
                to the device list, one of which a configuration enables. The device set is built when the
                service starts, so this takes effect at the next restart.</div>`
            : null
        }
      </div></div></div>
    <div class="card" style="max-width:560px"><div class="card-head"><h3>External console</h3></div>
      <div class="card-body">
        <p class="muted" style="margin:0 0 12px; font-size:var(--fs-1)">Where the real machine's console line is read; the dashboard shows that line.</p>
        <div class="set-grid">
          <div class="set-name">Source</div>
          <div class="set-val">
            ${(
              [
                ['ttys2', html`BBB <span class="mono">/dev/ttyS2</span>`],
                ['webserial', html`Web Serial`],
                ['off', 'Off'],
              ] as const
            ).map(
              ([v, lbl]) =>
                html`<label class="radio"><input type="radio" name="extsrc" value=${v} checked=${ec.source === v}
                onChange=${() => putSettings({ external_console: { source: v } }, 'console source set')} /> ${lbl}</label>`
            )}
          </div>
          <div class="set-info">ttyS2 reads a UART on the BeagleBone; Web Serial reads a USB serial port on the browser's own machine.</div>
          <div class="set-name">Baud</div>
          <div class="set-val"><select class="mono" value=${String(ec.baud || 38400)}
            onChange=${(e: Event) =>
              putSettings(
                { external_console: { baud: parseInt((e.target as HTMLSelectElement).value, 10) } },
                'baud set'
              )}>
            ${['300', '1200', '2400', '4800', '9600', '19200', '38400'].map(
              (b) => html`<option value=${b}>${b}</option>`
            )}</select></div>
          <div class="set-info">Line speed of the console line.</div>
          ${
            ec.source === 'webserial'
              ? html`<div class="set-name">Connection</div>
                <div class="set-val"><button class="btn small" onClick=${() => {
                  if (!('serial' in navigator)) return;
                  serialConnected()
                    ? serialDisconnect()
                    : serialConnect(parseInt(String(ec.baud || 38400), 10));
                }}>${serialConnected() ? 'Disconnect' : 'Connect'}</button></div>
                <div class="set-info">Web Serial needs a one-time grant to reach the browser machine's USB serial port.</div>`
              : null
          }
        </div></div></div>
  </section>`;
}
