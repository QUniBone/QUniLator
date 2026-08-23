// The image library as a hierarchical file manager at /storage. The whole tree
// is on screen at once: a folder row expands in place and its children appear
// indented beneath it, so an image can be dragged to a sibling folder, and a
// breadcrumb carries it to an ancestor. The selected folder — the one uploads
// and new images land in — is the path tail of the URL (/storage, /storage/du,
// /storage/dl/systems), which reveals it in the tree and keeps a link to it
// shareable.
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
  packageRoms,
  copyPackageRom,
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

// The folder a PROM card is programmed from, and the tree below it. The package
// ships the DEC M9312 listings under /usr/share, where nothing may reference
// them: they are offered here as something to copy in, and the copy is the
// operator's — attachable to a card, and editable.
const ROM_DIR = 'roms';
const isRomFolder = (path: string): boolean =>
  path === ROM_DIR || path.startsWith(ROM_DIR + '/');

// Which folders are open and which image rows show their file listing are the
// operator's view of the tree, so they are kept in localStorage and restored on
// the next visit and after a reload.
const OPEN_KEY = 'qbone.storage.open';
const CONTENTS_KEY = 'qbone.storage.contents';

type PathSet = Record<string, boolean>;

function loadPaths(key: string): PathSet {
  const out: PathSet = {};
  try {
    const raw = JSON.parse(localStorage.getItem(key) || '[]');
    if (Array.isArray(raw)) raw.forEach((p) => typeof p === 'string' && (out[p] = true));
  } catch {
    /* a corrupt entry starts over from an empty tree */
  }
  return out;
}

function savePaths(key: string, set: PathSet): void {
  try {
    localStorage.setItem(key, JSON.stringify(Object.keys(set).filter((k) => set[k])));
  } catch {
    /* storage full or blocked: the view still works, it just forgets */
  }
}

// What holds an image where it is, in words the operator can act on. An empty
// string means the file is free to move.
function moveBlocker(im: ImageInfo): string {
  const att = im.attached || [];
  if (att.length)
    return im.name + ' is mounted on ' + att.join(', ') + ' — detach the drive before moving it.';
  const used = im.used || [];
  if (used.length)
    return (
      im.name +
      ' is named by ' +
      used.map((u) => u.config + ':' + u.device).join(', ') +
      (used.length === 1 ? ' — that configuration' : ' — those configurations') +
      ' would lose it.'
    );
  return '';
}

// The drop target the pointer is over: a folder subpath, '' for the root, or
// null when the drag is over nothing that takes it.
type DropTarget = string | null;

interface DragProps {
  over: DropTarget;
  canDrop: (dir: string) => boolean;
  onOver: (e: DragEvent, dir: string) => void;
  onDrop: (e: DragEvent, dir: string) => void;
}

// The trail of ancestors above the listing. Each crumb selects its folder, and
// each crumb takes a dropped image, which is how a file deep in the tree
// reaches an ancestor or the root in one gesture.
function Breadcrumbs({ cwd, go, drag }: { cwd: string; go: (dir: string) => void; drag: DragProps }) {
  const segs = cwd ? cwd.split('/') : [];
  let acc = '';
  const crumb = (dir: string, label: string) =>
    html`<button
      class=${'crumb' + (drag.over === dir ? ' drop-into' : '') + (drag.canDrop(dir) ? ' drop-open' : '')}
      onClick=${() => go(dir)}
      onDragOver=${(e: DragEvent) => drag.onOver(e, dir)}
      onDrop=${(e: DragEvent) => drag.onDrop(e, dir)}
    >
      ${label}
    </button>`;
  return html`<nav class="crumbs">
    ${crumb('', 'root')}
    ${segs.map((seg) => {
      acc = acc ? acc + '/' + seg : seg;
      const here = acc;
      return html`<span class="crumb-sep">›</span>${crumb(here, seg)}`;
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

// An overlay that has taken no write is the resting state of every read-only
// pack a drive holds: nothing to discard, nothing to consolidate and no unsaved
// work to announce. The chip and the operations appear with the first write.
const overlayHasWrites = (im: ImageInfo) => !!im.overlay && (im.overlay_dirty_blocks ?? 0) > 0;

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
  const [expanded, setExpanded] = useState<PathSet>(() => loadPaths(CONTENTS_KEY));
  const [contents, setContents] = useState<Record<string, ImageContents>>({});
  // Which folder rows are open.
  const [openDirs, setOpenDirs] = useState<PathSet>(() => loadPaths(OPEN_KEY));

  useEffect(() => savePaths(OPEN_KEY, openDirs), [openDirs]);
  useEffect(() => savePaths(CONTENTS_KEY, expanded), [expanded]);

  // Rows restored as expanded need their listing read again — the decode is
  // cached in memory only.
  useEffect(() => {
    Object.keys(expanded)
      .filter((p) => expanded[p])
      .forEach((p) => imageContents(p).then((r) => setContents((c) => ({ ...c, [p]: r }))));
  }, []);

  const toggleContents = (subpath: string) => {
    const open = !expanded[subpath];
    setExpanded((e) => ({ ...e, [subpath]: open }));
    if (open && !contents[subpath]) {
      imageContents(subpath).then((r) => setContents((c) => ({ ...c, [subpath]: r })));
    }
  };

  const go = (dir: string) => loc.route(storageRoute(dir));

  const openPath = (dir: string) => {
    if (!dir) return;
    setOpenDirs((o) => {
      const next = { ...o };
      let acc = '';
      dir.split('/').forEach((seg) => {
        acc = acc ? acc + '/' + seg : seg;
        next[acc] = true;
      });
      return next;
    });
  };

  // The folder the URL names is revealed: every ancestor of it, and the folder
  // itself, opens.
  useEffect(() => openPath(cwd), [cwd]);

  // ---- the tree ----
  // The store holds every folder path and every image, so the listing is that
  // flat data grouped by parent and walked depth-first, stopping at any folder
  // the operator has closed.
  const dirKids = new Map<string, string[]>();
  (s.dirs || []).forEach((d) => {
    const p = parentDir(d);
    const list = dirKids.get(p);
    if (list) list.push(d);
    else dirKids.set(p, [d]);
  });
  const fileKids = new Map<string, ImageInfo[]>();
  (s.images || []).forEach((im) => {
    const list = fileKids.get(im.dir);
    if (list) list.push(im);
    else fileKids.set(im.dir, [im]);
  });
  dirKids.forEach((l) => l.sort());
  fileKids.forEach((l) => l.sort((a, b) => a.name.localeCompare(b.name)));

  type Row =
    | { kind: 'dir'; path: string; depth: number; children: boolean }
    | { kind: 'file'; im: ImageInfo; depth: number };

  const rows: Row[] = [];
  const walk = (dir: string, depth: number) => {
    (dirKids.get(dir) || []).forEach((d) => {
      const children = (dirKids.get(d) || []).length + (fileKids.get(d) || []).length > 0;
      rows.push({ kind: 'dir', path: d, depth, children });
      if (openDirs[d]) walk(d, depth + 1);
    });
    (fileKids.get(dir) || []).forEach((im) => rows.push({ kind: 'file', im, depth }));
  };
  walk('', 0);

  // ---- dragging an image into a folder ----
  // The image being dragged is held in a ref, because a dragover handler may
  // not read the dataTransfer and every event of the gesture — the first
  // dragover can arrive before the render that follows dragstart — has to see
  // it. The state copy alongside it is what the highlighting renders from.
  const dragRef = useRef<ImageInfo | null>(null);
  const [dragging, setDragging] = useState<ImageInfo | null>(null);
  const [over, setOver] = useState<DropTarget>(null);

  const canDrop = (dir: string) => !!dragRef.current && dragRef.current.dir !== dir;

  const endDrag = () => {
    dragRef.current = null;
    setDragging(null);
    setOver(null);
  };

  const dragOver = (e: DragEvent, dir: string) => {
    if (!canDrop(dir)) return; // no preventDefault: the pointer says "not here"
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'move';
    if (over !== dir) setOver(dir);
  };

  const dropInto = async (e: DragEvent, dir: string) => {
    e.preventDefault();
    const im = dragRef.current;
    endDrag();
    if (!im || im.dir === dir) return;
    if (await moveImage(im.path, dir ? dir + '/' + im.name : im.name)) openPath(dir);
  };

  const startDrag = (e: DragEvent, im: ImageInfo) => {
    // a drag begun on the Download link is the browser's own link drag
    if ((e.target as HTMLElement | null)?.closest('a')) return;
    const why = moveBlocker(im);
    if (why) {
      e.preventDefault();
      toast('move ' + im.name, why);
      return;
    }
    dragRef.current = im;
    setDragging(im);
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', im.path);
    }
  };

  const dragProps: DragProps = { over, canDrop, onOver: dragOver, onDrop: dropInto };

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

  // Copy one of the ROM listings the package ships into this folder. The
  // package tree is never named by a device — an upgrade rewrites it — so the
  // operator takes a copy and that copy is what a card is programmed from. A
  // listing names itself on its ".title" line, which is what makes a 23-nnnnn
  // part number recognisable.
  const copyDefaultRom = async (dir: string) => {
    const roms = await packageRoms();
    const options = roms.map((r) => {
      const title = r.title || r.name;
      const category = /boot/i.test(title)
        ? 'boot PROM'
        : /console|diag/i.test(title)
          ? 'console & diagnostic PROM'
          : 'PROM';
      return { name: r.name, label: title, type: 'rom', category };
    });
    const pick = await pickDevice('Copy a ROM into ' + dir, options, {
      noun: 'ROM',
      empty: 'This QUniLator carries no packaged ROMs.',
    });
    if (!pick) return;
    await copyPackageRom(pick, dir);
  };

  // A blank medium to write on. What is on offer comes from the drives the
  // machine can carry: each publishes the capacity of the medium it takes, so
  // the list is what this QUniLator knows about rather than a table kept in step by
  // hand. A tape carries no size — a blank reel is a file mark and nothing else.
  // A controller that reads the size off the image instead of fixing it (MSCP
  // with useimagesize) will take any disk, which is what the custom entry is for.
  // the sentinel rides through an HTML data attribute, so it has to be plain
  // text that no drive type can be
  const CUSTOM = '*custom';
  const newImage = async () => {
    const media = new Map<string, { name: string; label: string; type: string; size: number }>();
    flatDevices().forEach((d) => {
      if (!d.params.some((p) => p.c === 'image') || media.has(d.type)) return;
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

  // Clicking a folder selects it — uploads and new images go there — and opens
  // it; clicking the folder already selected collapses and expands it.
  const pickFolder = (d: string) => {
    if (d === cwd) setOpenDirs((o) => ({ ...o, [d]: !o[d] }));
    else {
      openPath(d);
      go(d);
    }
  };

  const indent = (depth: number) => 'padding-left:' + (10 + depth * 20) + 'px';

  const folderRow = (path: string, depth: number, children: boolean) => {
    const isOpen = !!openDirs[path];
    const target = over === path;
    return html`<tr key=${'d:' + path}
      class=${'row-folder' + (cwd === path ? ' row-current' : '') + (target ? ' drop-into' : '') +
        (canDrop(path) ? ' drop-open' : '')}
      onDragOver=${(e: DragEvent) => dragOver(e, path)}
      onDrop=${(e: DragEvent) => dropInto(e, path)}>
      <td style=${indent(depth)}><span class="tree-name">
        <button class=${'contents-toggle' + (isOpen ? ' open' : '')}
          title=${isOpen ? 'Collapse folder' : 'Expand folder'} aria-expanded=${isOpen}
          disabled=${!children}
          onClick=${() => setOpenDirs((o) => ({ ...o, [path]: !o[path] }))}>▸</button>
        <button class="tree-name" onClick=${() => pickFolder(path)}>
          <span class="pick-icon">${isOpen && children ? '📂' : '📁'}</span>
          <span class="mono">${baseName(path)}</span></button></span></td>
      <td class="muted">—</td>
      <td class="muted">${target ? html`<span class="drop-hint">drop to move here</span>` : 'folder'}</td>
      <td class="muted">—</td>
      <td style="text-align:right; white-space:nowrap">
        ${isRomFolder(path)
          ? html`<button class="btn small" title="Copy one of the ROM listings the package ships into this folder"
              onClick=${() => copyDefaultRom(path)}>Copy default ROM…</button>${' '}`
          : null}
        <button class="btn small" onClick=${() => renameEntry(path, true)}>Rename…</button>${' '}
        <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${() => removeFolder(path)} />
      </td></tr>`;
  };

  const fileRow = (im: ImageInfo, depth: number) => {
    const open = !!expanded[im.path];
    const held = moveBlocker(im);
    return html`<${'tr'} key=${'f:' + im.path} draggable=${true}
      class=${'row-image' + (held ? ' held' : '') + (dragging?.path === im.path ? ' dragging' : '')}
      title=${held || undefined}
      onDragStart=${(e: DragEvent) => startDrag(e, im)} onDragEnd=${endDrag}>
      <td style=${indent(depth)}><span class="tree-name">
        <button class=${'contents-toggle' + (open ? ' open' : '')}
          title=${open ? 'Hide file listing' : 'Show file listing'}
          aria-expanded=${open} onClick=${() => toggleContents(im.path)}>▸</button>
        <span class="pick-icon">💾</span>
        <span class="mono">${im.name}</span>
        ${im.writable === false ? html`<span class="chip off" title="read-only file">read-only</span>` : null}
        ${overlayHasWrites(im) ? html`<span class="chip warn" title="a copy-on-write overlay holds unsaved writes">overlay</span>` : null}</span></td>
      <td class="mono" style="white-space:nowrap">${humanSize(im.size)}</td>
      <td><${ImageUsage} im=${im} /></td>
      <td class="muted mono" style="font-size:var(--fs-0); white-space:nowrap">${im.mtime}</td>
      <td style="text-align:right; white-space:nowrap">
        <button class="btn small" onClick=${() => toggleContents(im.path)}>${open ? 'Hide' : 'Contents'}</button>${' '}
        <a class="btn small" href=${subURL('/api/images/', im.path)} download=${im.name}>Download</a>${' '}
        <button class="btn small" onClick=${() => renameEntry(im.path, false)}>Rename…</button>${' '}
        <${DelButton} label="Delete" confirmLabel="Confirm delete" onConfirm=${() => deleteImage(im.path)} />
      </td></tr>
      ${overlayHasWrites(im)
        ? html`<tr key=${'ov:' + im.path} class="overlay-row"><td colspan="5" style=${indent(depth + 1)}>
            <${OverlayPanel} im=${im} /></td></tr>`
        : null}
      ${open
        ? html`<tr key=${'fc:' + im.path} class="contents-row"><td colspan="5" style=${indent(depth + 1)}>
            <${ContentsPanel} data=${contents[im.path]} /></td></tr>`
        : null}`;
  };

  return html`<section class="page active" data-page="storage" onDragEnd=${endDrag}>
    <div class="store-toolbar">
      <${Breadcrumbs} cwd=${cwd} go=${go} drag=${dragProps} />
      <span class="spacer"></span>
      <button class="btn small" onClick=${newFolder}>New folder</button>
      <button class="btn small primary" onClick=${newImage}>New image…</button>
    </div>

    <div class=${'dropzone' + (busy ? ' busy' : '')} onClick=${() => fileRef.current?.click()}
      onDragOver=${(e: DragEvent) => {
        // files from the desktop only; a row dragged within the tree passes over
        if (e.dataTransfer && Array.from(e.dataTransfer.types).includes('Files')) e.preventDefault();
      }}
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
        ${rows.map((r) =>
          r.kind === 'dir' ? folderRow(r.path, r.depth, r.children) : fileRow(r.im, r.depth)
        )}
        ${
          !rows.length
            ? html`<tr><td colspan="5" class="muted">The library is empty — drop a disk image above, or make a folder.</td></tr>`
            : null
        }
      </tbody></table></div></div></section>`;
}
