import { html } from '../html';
import { useState, useRef, useEffect } from 'preact/hooks';
import { humanSize } from '../lib/util';
import { toast } from '../lib/toast';
import { refreshImages } from '../api';
import { useStore } from '../store';
import { DelButton } from './common';
import type { ImageInfo } from '../types';

function uploadImage(file: File, setStatus: (s: string) => void): void {
  const form = new FormData();
  form.append('file', file, file.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/images');
  xhr.upload.onprogress = (ev) => {
    if (ev.lengthComputable)
      setStatus('Uploading ' + file.name + ' — ' + Math.round((100 * ev.loaded) / ev.total) + '%');
  };
  xhr.onload = () => {
    setStatus(xhr.status === 200 ? 'Uploaded ' + file.name : 'Upload failed: ' + xhr.responseText);
    toast('POST /api/images', xhr.status === 200 ? file.name + ' uploaded' : 'upload failed');
    refreshImages().catch(() => {});
  };
  xhr.onerror = () => setStatus('Upload failed.');
  xhr.send(form);
}

function ImageUsage({ im }: { im: ImageInfo }) {
  const parts: unknown[] = [];
  (im.used || []).forEach((u) =>
    parts.push(
      html`<span class="chip out mono" title="named by a saved configuration">${u.config}:${u.device}</span>`
    )
  );
  (im.attached || []).forEach((d) =>
    parts.push(html`<span class="chip ok mono" title="mounted on this drive now">${d} mounted</span>`)
  );
  return parts.length ? html`${parts}` : html`<span class="muted">—</span>`;
}

export function StoragePage() {
  const s = useStore();
  useEffect(() => {
    refreshImages().catch(() => {});
  }, []);
  const [status, setStatus] = useState('Drop an image file here to upload, or click to choose · ');
  const fileRef = useRef<HTMLInputElement | null>(null);
  const del = (name: string) =>
    fetch('/api/images/' + encodeURIComponent(name), { method: 'DELETE' }).then(async (r) => {
      const data = await r.json().catch(() => ({}));
      toast('DELETE /api/images/' + name, r.ok ? 'image deleted' : data.error || 'delete failed');
      refreshImages().catch(() => {});
    });
  return html`<section class="page active" data-page="storage">
    <div class="dropzone" onClick=${() => fileRef.current?.click()}
      onDragOver=${(e: Event) => e.preventDefault()}
      onDrop=${(e: DragEvent) => {
        e.preventDefault();
        if (e.dataTransfer && e.dataTransfer.files.length) uploadImage(e.dataTransfer.files[0], setStatus);
      }}>
      ${status}<span class="mono">.rl02 .rl01 .rk05 .rx2 .dsk .tap (.gz ok)</span></div>
    <input type="file" hidden ref=${fileRef}
      onChange=${(e: Event) => {
        const inp = e.target as HTMLInputElement;
        if (inp.files && inp.files.length) uploadImage(inp.files[0], setStatus);
        inp.value = '';
      }} />
    <div class="card"><div class="table-wrap"><table class="data">
      <thead><tr><th>Image</th><th class="num">Size</th><th>Used by</th><th>Modified</th><th></th></tr></thead>
      <tbody>${
        s.images.length
          ? s.images.map(
              (im) => html`<tr key=${im.name}>
        <td class="mono">${im.name}</td>
        <td class="mono">${humanSize(im.size)}</td>
        <td><${ImageUsage} im=${im} /></td>
        <td class="muted mono" style="font-size:var(--fs-0)">${im.mtime}</td>
        <td style="text-align:right; white-space:nowrap">
          <a class="btn small" href=${'/api/images/' + encodeURIComponent(im.name)} download>Download</a>${' '}
          <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${() => del(im.name)} />
        </td></tr>`
            )
          : html`<tr><td colspan="5" class="muted">No images yet — drop a disk image above to upload it.</td></tr>`
      }
      </tbody></table></div></div></section>`;
}
