import { html } from '../html';
import { useState, useEffect } from 'preact/hooks';
import { useRoute, useLocation } from 'preact-iso';
import { esc, imageLabel } from '../lib/util';
import { toast } from '../lib/toast';
import { apiJSON, cfgDriveNames, loadConfigs, refreshDevices } from '../api';
import { confirmModal, pickImage } from '../lib/modals';
import { useStore } from '../store';
import { DelButton } from './common';
import type { ConfigSnapshot, ConfigSummary } from '../types';

async function setConfigImage(cfgName: string, drive: string, current: string): Promise<void> {
  const name = await pickImage(
    'Image for ' + drive + ' in “' + cfgName + '”',
    'No image — leave the drive empty',
    current
  );
  if (name === null) return;
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' +
      encodeURIComponent(cfgName) +
      '/devices/' +
      encodeURIComponent(drive) +
      '/image',
    {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value: name }),
    }
  );
  toast(cfgName + ':' + drive + ' = ' + (name || '""'), res.ok ? 'configuration updated' : res.data.error || 'rejected');
  await loadConfigs().catch(() => {});
}

function CfgDevices({ cfgName, snap }: { cfgName: string; snap: ConfigSnapshot | null | undefined }) {
  const devs = ((snap || { devices: [] }).devices || []).filter((d) => d.enabled);
  if (!devs.length)
    return html`<div class="muted" style="padding:10px 14px; font-size:var(--fs-1)">This configuration switches every device off.</div>`;
  const drives = cfgDriveNames();
  return devs.map((d) => {
    const names = Object.keys(d.params || {})
      .sort()
      .filter((n) => n !== 'image');
    const takesImage = drives.indexOf(d.name) !== -1;
    const img = (d.params || {}).image || '';
    return html`<div key=${d.name}>
      <div class="dev-sub"><span class="devname">${d.name}</span><span class="spacer"></span><span class="chip ok">enabled</span></div>
      <div class="params">${
        names.length || takesImage
          ? html`<div class="p-grid">
            ${
              takesImage
                ? html`<div class="p-name">image</div>
              <div class="p-val"><button class="btn small" onClick=${() =>
                setConfigImage(cfgName, d.name, img)}>
                ${img ? imageLabel(img) : 'no image'} · change…</button></div>
              <div class="p-info">the medium this drive starts with</div>`
                : null
            }
            ${names.map(
              (n) => html`<div class="p-name">${n}</div>
              <div class="p-val"><span class="ro">${String((d.params || {})[n])}</span></div><div class="p-info"></div>`
            )}</div>`
          : html`<span class="muted" style="font-size:var(--fs-1)">all parameters at their defaults</span>`
      }</div></div>`;
  });
}

function CfgRow({ c, halted, open }: { c: ConfigSummary; halted: boolean; open: boolean }) {
  const loc = useLocation();
  const apply = async () => {
    if (
      !halted &&
      !(await confirmModal(
        'Apply while the PDP-11 is running?',
        'The CPU is running. Applying <b>' +
          esc(c.name) +
          '</b> reconfigures every device — drives are detached ' +
          'and reattached, and the running system will not survive it.',
        'Apply anyway'
      ))
    )
      return;
    const res = await apiJSON<{ ok?: boolean; errors?: string[] }>(
      '/api/configs/' + encodeURIComponent(c.name) + '/apply',
      { method: 'POST' }
    );
    toast(
      'POST /api/configs/' + c.name + '/apply',
      res.ok && res.data.ok
        ? 'configuration applied'
        : 'applied with rejections: ' + ((res.data.errors || []).join('; ') || 'request failed')
    );
    refreshDevices().catch(() => {});
    loadConfigs().catch(() => {});
  };
  const del = () =>
    fetch('/api/configs/' + encodeURIComponent(c.name), { method: 'DELETE' }).then((r) => {
      toast('DELETE /api/configs/' + c.name, r.ok ? 'configuration deleted' : 'delete failed');
      loadConfigs().catch(() => {});
    });
  return html`<div class="card cfg-row">
    <div class="card-head">
      <h3>${c.name}</h3>
      <span class="muted" style="font-size:var(--fs-0)">${c.mtime}</span>
      ${c.loaded ? html`<span class="chip ok">loaded</span>` : null}
      ${(c.enabled || []).map((d) => html`<span class="chip out">${d}</span>`)}
      <span style="margin-left:auto; display:flex; gap:8px">
        <button class="btn small" onClick=${() => loc.route(open ? '/config' : '/config/' + encodeURIComponent(c.name))}>${open ? 'Hide' : 'View'}</button>
        <button class="btn small primary" disabled=${c.loaded} title=${c.loaded ? 'already loaded' : ''} onClick=${apply}>Apply</button>
        <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${del} />
      </span></div>
    ${open ? html`<div class="cfg-devs"><${CfgDevices} cfgName=${c.name} snap=${c.snapshot} /></div>` : null}
  </div>`;
}

export function ConfigsPage() {
  const s = useStore();
  useEffect(() => {
    loadConfigs().catch(() => {});
  }, []);
  const { params } = useRoute();
  const selected = params.name ? decodeURIComponent(params.name) : '';
  const [name, setName] = useState('');
  const configs = s.configs || [];
  const alreadySaved = configs.some((c) => c.loaded);
  const save = async () => {
    if (!name.trim()) {
      toast('PUT /api/configs/…', 'enter a configuration name first');
      return;
    }
    const r = await fetch('/api/configs/' + encodeURIComponent(name.trim()), { method: 'PUT' });
    toast('PUT /api/configs/' + name.trim(), r.ok ? 'current setup saved' : 'save failed');
    setName('');
    loadConfigs().catch(() => {});
  };
  return html`<section class="page active" data-page="configurations"><div>
    ${
      !alreadySaved
        ? html`<div class="card cfg-row"><div class="card-head">
      <h3>Save current setup</h3>
      <span style="margin-left:auto; display:flex; gap:8px">
        <input type="text" placeholder="configuration name" class="mono" value=${name}
          onInput=${(e: Event) => setName((e.target as HTMLInputElement).value)}
          style="background:transparent; border:1px solid var(--line); border-radius:6px; padding:4px 8px; color:inherit" />
        <button class="btn small primary" onClick=${save}>Save</button></span></div></div>`
        : null
    }
    ${configs.map((c) => html`<${CfgRow} c=${c} halted=${s.bus.halted} open=${selected === c.name} key=${c.name} />`)}
    ${
      configs.length === 0 && s.configs != null
        ? html`<div class="muted" style="padding:8px">No saved configurations yet.</div>`
        : null
    }
  </div></section>`;
}
