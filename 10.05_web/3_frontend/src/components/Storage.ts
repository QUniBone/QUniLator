// The image library as a hierarchical file manager at /storage. The current
// folder is the path tail of the URL (/storage, /storage/du, /storage/dl/systems),
// so navigation is shareable and the browser's back button walks the tree.
import { html } from '../html';
import { useState, useRef, useEffect } from 'preact/hooks';
import { useRoute, useLocation } from 'preact-iso';
import { humanSize, baseName, parentDir, subURL, esc } from '../lib/util';
import {
  refreshImages,
  uploadImages,
  deleteImage,
  createFolder,
  deleteFolder,
  moveImage,
  imageContents,
  discardOverlay,
  commitOverlay,
  exportOverlay,
  createImage,
  type OverlayResult,
} from '../api';
import { promptModal, confirmModal, pickDevice } from '../lib/modals';
import { flatDevices } from '../lib/devmodel';
import { useStore } from '../store';
import { DelButton } from './common';
import { toast } from '../lib/toast';
import type { ImageInfo, ImageContents } from '../types';

// A size the operator typed: a bare number is 512-byte blocks, the way a drive's
// capacity is quoted; K/M/G are binary multiples of bytes. 0 means it made no
// sense, which the caller reports rather than guessing.
export function parseSize(text: string): number {
  const m = /^\s*([0-9]*\.?[0-9]+)\s*([kKmMgG])?[bB]?\s*$/.exec(text);
  if (!m) return 0;
  const n = parseFloat(m[1]);
  if (!isFinite(n) || n <= 0) return 0;
  const unit = (m[2] || '').toLowerCase();
  const scale = unit === 'k' ? 1024 : unit === 'm' ? 1024 * 1024
    : unit === 'g' ? 1024 * 1024 * 1024 : 512; // no unit: blocks
  return Math.round(n * scale);
}

const storageRoute = (cwd: string): string =>
  cwd ? subURL('/storage/', cwd) : '/storage';

function Breadcrumbs({ cwd, go }: { cwd: string; go: (dir: string) => void }) {
  const segs = cwd ? cwd.split('/') : [];
  let acc = '';
  return html`<nav class="crumbs">
    <button class="crumb" onClick=${() => go('')}>root</button>
    ${segs.map((seg) => {
      acc = acc ? acc + '/' + seg : seg;
      const here = acc;
      return html`<span class="crumb-sep">›</span>
        <button class="crumb" onClick=${() => go(here)}>${seg}</button>`;
    })}
  </nav>`;
}

// The config:device references and live drive attachments a file carries.
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

// The decoded file listing of one image, rendered under its row when expanded.
// `data` is undefined while the fetch is in flight.
function ContentsPanel({ data }: { data?: ImageContents }) {
  if (!data)
    return html`<div class="img-contents"><span class="muted">reading…</span></div>`;

  const fs = data.filesystem;
  const recognized = fs === 'RT-11' || fs === 'ODS-2';
  const files = data.files || [];
  const vol = data.home && typeof data.home.volume_name === 'string' ? data.home.volume_name : '';

  const head = html`<div class="img-contents-head">
    <span class="fs mono">${fs}</span>
    ${vol ? html`<span class="vol mono">${vol}</span>` : null}
  </div>`;

  if (!recognized)
    return html`<div class="img-contents">${head}
      <div class="muted note">not a recognized filesystem (RT-11 / ODS-2)</div></div>`;

  if (!files.length)
    return html`<div class="img-contents">${head}
      <div class="muted note">empty volume — no files</div></div>`;

  if (fs === 'RT-11')
    return html`<div class="img-contents">${head}
      <div class="table-wrap"><table class="data inner">
        <thead><tr><th>File</th><th class="num">Size</th><th class="num">Blocks</th><th>Date</th></tr></thead>
        <tbody>${files.map(
          (f, i) => html`<tr key=${'c:' + i}>
            <td class="mono">${f.name}</td>
            <td class="mono">${f.bytes != null ? humanSize(f.bytes) : '—'}</td>
            <td class="mono">${f.blocks != null ? f.blocks : '—'}</td>
            <td class="muted mono">${f.date || '—'}</td></tr>`
        )}</tbody></table></div></div>`;

  // ODS-2: show the directory column only when the files span more than one.
  const dirs = new Set(files.map((f) => f.directory).filter(Boolean));
  const showDir = dirs.size > 1;
  return html`<div class="img-contents">${head}
    <div class="table-wrap"><table class="data inner">
      <thead><tr><th>File</th>${showDir ? html`<th>Dir</th>` : null}
        <th class="num">Size</th><th class="num">Blocks</th><th>Created</th></tr></thead>
      <tbody>${files.map(
        (f, i) => html`<tr key=${'c:' + i}>
          <td class="mono">${f.name}</td>
          ${showDir ? html`<td class="mono muted">${f.directory || '—'}</td>` : null}
          <td class="mono">${f.size_bytes != null ? humanSize(f.size_bytes) : '—'}</td>
          <td class="mono">${f.blocks_on_volume != null ? f.blocks_on_volume : '—'}</td>
          <td class="muted mono">${f.created || '—'}</td></tr>`
      )}</tbody></table></div></div>`;
}

// The copy-on-write overlay readout and its three operations, shown beneath a
// disk row whose image carries an active overlay. Discard reverts to the base;
// consolidate folds the writes into the base (destructive) or into a fresh file
// (non-destructive). All three need the machine halted; the backend's 409 is
// surfaced inline rather than as a passing toast.
function OverlayPanel({ im }: { im: ImageInfo }) {
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState('');

  const react = (r: OverlayResult) => {
    if (r.ok) setMsg('');
    else if (r.status === 409)
      setMsg('Halt the machine first — overlay operations need the disk idle.');
    else setMsg(r.error || 'overlay operation failed');
  };

  const run = async (fn: () => Promise<OverlayResult>) => {
    setBusy(true);
    react(await fn());
    setBusy(false);
  };

  const discard = async () => {
    if (!(await confirmModal('Discard overlay', 'Discard all changes since the base image?', 'Discard'))) return;
    run(() => discardOverlay(im.path));
  };

  const consolidate = async () => {
    const ok = await confirmModal(
      'Consolidate into the base image',
      'Permanently write these changes into <span class="mono">' +
        esc(im.name) +
        '</span>? The original cannot be recovered afterward.',
      'Consolidate'
    );
    if (!ok) return;
    run(() => commitOverlay(im.path));
  };

  const exportNew = async () => {
    const dot = im.path.lastIndexOf('.');
    const suggestion =
      dot > im.path.lastIndexOf('/')
        ? im.path.slice(0, dot) + '-flat' + im.path.slice(dot)
        : im.path + '-flat';
    const dest = await promptModal(
      'Consolidate to a new image',
      'Destination path (in the image tree)',
      suggestion,
      'Export'
    );
    if (!dest) return;
    run(() => exportOverlay(im.path, dest));
  };

  const blocks = im.overlay_dirty_blocks ?? 0;
  return html`<div class="overlay-panel">
    <div class="overlay-status">
      <span class="overlay-label">Overlay active</span>
      <span class="muted mono">${blocks.toLocaleString()} block${blocks === 1 ? '' : 's'} written · ${humanSize(im.overlay_bytes ?? 0)}</span>
    </div>
    <div class="overlay-actions">
      <button class="btn small" disabled=${busy} onClick=${discard}>Discard overlay</button>
      <button class="btn small" disabled=${busy} onClick=${consolidate}>Consolidate → base</button>
      <button class="btn small" disabled=${busy} onClick=${exportNew}>Consolidate → new file…</button>
    </div>
    ${msg ? html`<div class="overlay-msg">${msg}</div>` : null}
  </div>`;
}

export function StoragePage() {
  const s = useStore();
  const loc = useLocation();
  const { params } = useRoute();
  // preact-iso decodes each rest segment already, so params.path is the subpath
  const cwd = params.path || '';

  useEffect(() => {
    refreshImages().catch(() => {});
  }, []);

  const [status, setStatus] = useState('Drop image files here to upload, or click to choose · ');
  const [busy, setBusy] = useState(false);
  const fileRef = useRef<HTMLInputElement | null>(null);

  // Which image rows have their file-listing expanded, and the decoded results
  // cached by subpath (fetched once on first expand).
  const [expanded, setExpanded] = useState<Record<string, boolean>>({});
  const [contents, setContents] = useState<Record<string, ImageContents>>({});

  const toggleContents = (subpath: string) => {
    const open = !expanded[subpath];
    setExpanded((e) => ({ ...e, [subpath]: open }));
    if (open && !contents[subpath]) {
      imageContents(subpath).then((r) => setContents((c) => ({ ...c, [subpath]: r })));
    }
  };

  const go = (dir: string) => loc.route(storageRoute(dir));

  const folders = (s.dirs || []).filter((d) => parentDir(d) === cwd).sort();
  const files = (s.images || [])
    .filter((im) => im.dir === cwd)
    .sort((a, b) => a.name.localeCompare(b.name));

  const upload = (list: FileList | File[]) => {
    const arr = Array.from(list);
    if (!arr.length) return;
    setBusy(true);
    setStatus('Uploading ' + arr.length + (arr.length === 1 ? ' file…' : ' files…'));
    uploadImages(arr, cwd, (f) =>
      setStatus('Uploading ' + arr.length + (arr.length === 1 ? ' file' : ' files') + ' — ' + Math.round(100 * f) + '%')
    ).then((r) => {
      setBusy(false);
      setStatus(
        r.ok
          ? 'Uploaded ' + r.names.join(', ') + '. Drop more here, or click to choose · '
          : 'Upload failed: ' + (r.error || 'error') + ' · '
      );
    });
  };

  const newFolder = async () => {
    const name = await promptModal('New folder', 'Folder name', '', 'Create');
    if (!name) return;
    const path = cwd ? cwd + '/' + name : name;
    await createFolder(path);
  };

  // A blank medium to write on. What is on offer comes from the drives the
  // machine can carry: each publishes the capacity of the medium it takes, so
  // the list is what this board knows about rather than a table kept in step by
  // hand. A tape carries no size — a blank reel is a file mark and nothing else.
  // A controller that reads the size off the image instead of fixing it (MSCP
  // with useimagesize) will take any disk, which is what the custom entry is for.
  // the sentinel rides through an HTML data attribute, so it has to be plain
  // text that no drive type can be
  const CUSTOM = '*custom';
  const newImage = async () => {
    const media = new Map<string, { name: string; label: string; type: string; size: number }>();
    flatDevices().forEach((d) => {
      if (!d.params.some((p) => p.n === 'image') || media.has(d.type)) return;
      const tape = d.category === 'tape';
      const cap = d.params.find((p) => p.n === 'capacity');
      const size = tape ? 0 : Number(cap ? cap.v : 0);
      if (!tape && !size) return; // a drive that does not say how big its medium is
      media.set(d.type, {
        name: d.type,
        label: d.type + (tape ? ' tape — a blank reel' : ' disk — ' + humanSize(size)),
        type: tape ? 'tape' : 'disk',
        size,
      });
    });
    const list = Array.from(media.values()).sort((a, b) => a.name.localeCompare(b.name));
    list.push({ name: CUSTOM, label: 'Disk of a size you name…', type: 'disk', size: 0 });
    const pick = await pickDevice('Blank medium', list);
    if (!pick) return;

    let kind: 'disk' | 'tape' = 'disk';
    let size = 0;
    let suffix = '.dsk';
    if (pick === CUSTOM) {
      const answer = await promptModal(
        'Disk of a size you name',
        'Size — a number of blocks, or a size like 40M or 1.5G',
        '20M',
        'Next'
      );
      if (!answer) return;
      size = parseSize(answer);
      if (!size) {
        toast('new image', '"' + answer + '" is not a size');
        return;
      }
    } else {
      const medium = media.get(pick);
      if (!medium) return;
      kind = medium.type === 'tape' ? 'tape' : 'disk';
      size = medium.size;
      suffix = kind === 'tape' ? '.tap' : '.' + pick.toLowerCase();
    }
    const name = await promptModal(
      pick === CUSTOM ? 'New ' + humanSize(size) + ' disk image' : 'New ' + pick + ' image',
      'File name',
      'scratch' + suffix,
      'Create',
      'stem'
    );
    if (!name) return;
    await createImage(name, cwd, kind, size);
  };

  const renameEntry = async (from: string, isFolder: boolean) => {
    const to = await promptModal(
      isFolder ? 'Rename or move folder' : 'Rename or move image',
      'New path (relative to the image root)',
      from,
      'Move'
    );
    if (!to || to === from) return;
    await moveImage(from, to);
    // a folder the current view sits inside may have moved out from under us
    if (isFolder && (cwd === from || cwd.startsWith(from + '/'))) go(parentDir(from));
  };

  const removeFolder = async (path: string) => {
    await deleteFolder(path);
  };

  return html`<section class="page active" data-page="storage">
    <div class="store-toolbar">
      <${Breadcrumbs} cwd=${cwd} go=${go} />
      <span class="spacer"></span>
      <button class="btn small" onClick=${newFolder}>New folder</button>
      <button class="btn small primary" onClick=${newImage}>New image…</button>
    </div>

    <div class=${'dropzone' + (busy ? ' busy' : '')} onClick=${() => fileRef.current?.click()}
      onDragOver=${(e: Event) => e.preventDefault()}
      onDrop=${(e: DragEvent) => {
        e.preventDefault();
        if (e.dataTransfer && e.dataTransfer.files.length) upload(e.dataTransfer.files);
      }}>
      ${status}<span class="mono">.rl02 .rl01 .rk05 .rx2 .dsk .tap (.gz ok)</span>
      <span class="muted"> → ${cwd ? html`<span class="mono">${cwd}</span>` : 'root'}</span></div>
    <input type="file" hidden multiple ref=${fileRef}
      onChange=${(e: Event) => {
        const inp = e.target as HTMLInputElement;
        if (inp.files && inp.files.length) upload(inp.files);
        inp.value = '';
      }} />

    <div class="card"><div class="table-wrap"><table class="data">
      <thead><tr><th>Name</th><th class="num">Size</th><th>Used by</th><th>Modified</th><th></th></tr></thead>
      <tbody>
        ${folders.map(
          (d) => html`<tr key=${'d:' + d} class="row-folder">
          <td><button class="tree-name" onClick=${() => go(d)}><span class="pick-icon">📁</span>
            <span class="mono">${baseName(d)}</span></button></td>
          <td class="muted">—</td>
          <td class="muted">folder</td>
          <td class="muted">—</td>
          <td style="text-align:right; white-space:nowrap">
            <button class="btn small" onClick=${() => renameEntry(d, true)}>Rename…</button>${' '}
            <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${() => removeFolder(d)} />
          </td></tr>`
        )}
        ${files.map((im) => {
          const open = !!expanded[im.path];
          return html`<tr key=${'f:' + im.path}>
          <td><span class="tree-name">
            <button class=${'contents-toggle' + (open ? ' open' : '')}
              title=${open ? 'Hide file listing' : 'Show file listing'}
              aria-expanded=${open} onClick=${() => toggleContents(im.path)}>▸</button>
            <span class="pick-icon">💾</span>
            <span class="mono">${im.name}</span>
            ${im.writable === false ? html`<span class="chip off" title="read-only file">read-only</span>` : null}
            ${im.overlay ? html`<span class="chip warn" title="a copy-on-write overlay holds unsaved writes">overlay</span>` : null}</span></td>
          <td class="mono">${humanSize(im.size)}</td>
          <td><${ImageUsage} im=${im} /></td>
          <td class="muted mono" style="font-size:var(--fs-0)">${im.mtime}</td>
          <td style="text-align:right; white-space:nowrap">
            <button class="btn small" onClick=${() => toggleContents(im.path)}>${open ? 'Hide' : 'Contents'}</button>${' '}
            <a class="btn small" href=${subURL('/api/images/', im.path)} download>Download</a>${' '}
            <button class="btn small" onClick=${() => renameEntry(im.path, false)}>Rename…</button>${' '}
            <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${() => deleteImage(im.path)} />
          </td></tr>
          ${im.overlay
            ? html`<tr key=${'ov:' + im.path} class="overlay-row"><td colspan="5">
                <${OverlayPanel} im=${im} /></td></tr>`
            : null}
          ${open
            ? html`<tr key=${'fc:' + im.path} class="contents-row"><td colspan="5">
                <${ContentsPanel} data=${contents[im.path]} /></td></tr>`
            : null}`;
        })}
        ${
          !folders.length && !files.length
            ? html`<tr><td colspan="5" class="muted">This folder is empty — drop a disk image above, or make a subfolder.</td></tr>`
            : null
        }
      </tbody></table></div></div></section>`;
}
