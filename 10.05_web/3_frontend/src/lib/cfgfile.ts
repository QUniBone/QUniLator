// Carrying a configuration off the board, and bringing one back.
//
// A configuration document travels on its own, and the board renders it for
// download. A configuration plus the media its drives hold travels as a zip,
// and that is built here rather than on the board: the browser already has
// every piece — it fetches the document and each image — and the archive is
// assembled with what the platform provides, so the appliance carries no
// archiving code and the bundle carries no library.
//
// Written entries are stored, not deflated. An import reads either form: a
// stored entry is the bytes themselves, and a deflated one goes through
// DecompressionStream, so an archive repacked by a zip tool still opens.

const encoder = new TextEncoder();

export interface ZipEntry {
  name: string;
  data: Uint8Array;
}

// CRC-32, the one a zip's directory carries. The table is built once.
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++)
    c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

class ByteWriter {
  private parts: Uint8Array[] = [];
  length = 0;
  push(b: Uint8Array): void {
    this.parts.push(b);
    this.length += b.length;
  }
  u16(v: number): void {
    this.push(new Uint8Array([v & 0xff, (v >>> 8) & 0xff]));
  }
  u32(v: number): void {
    this.push(
      new Uint8Array([v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff])
    );
  }
  blob(type: string): Blob {
    return new Blob(this.parts as BlobPart[], { type });
  }
}

/** Build a zip holding these entries, stored. */
export function makeZip(entries: ZipEntry[]): Blob {
  const out = new ByteWriter();
  const central: { name: Uint8Array; crc: number; size: number; offset: number }[] = [];
  for (const e of entries) {
    const name = encoder.encode(e.name);
    const crc = crc32(e.data);
    central.push({ name, crc, size: e.data.length, offset: out.length });
    out.u32(0x04034b50); // local file header
    out.u16(20); // version needed
    out.u16(0); // flags
    out.u16(0); // stored
    out.u16(0); // time
    out.u16(0); // date
    out.u32(crc);
    out.u32(e.data.length); // compressed
    out.u32(e.data.length); // uncompressed
    out.u16(name.length);
    out.u16(0); // extra
    out.push(name);
    out.push(e.data);
  }
  const dirStart = out.length;
  for (const c of central) {
    out.u32(0x02014b50); // central directory header
    out.u16(20); // version made by
    out.u16(20); // version needed
    out.u16(0);
    out.u16(0); // stored
    out.u16(0);
    out.u16(0);
    out.u32(c.crc);
    out.u32(c.size);
    out.u32(c.size);
    out.u16(c.name.length);
    out.u16(0);
    out.u16(0);
    out.u16(0);
    out.u16(0);
    out.u32(0);
    out.u32(c.offset);
    out.push(c.name);
  }
  const dirSize = out.length - dirStart;
  out.u32(0x06054b50); // end of central directory
  out.u16(0);
  out.u16(0);
  out.u16(central.length);
  out.u16(central.length);
  out.u32(dirSize);
  out.u32(dirStart);
  out.u16(0);
  return out.blob('application/zip');
}

async function inflateRaw(bytes: Uint8Array): Promise<Uint8Array> {
  const ds = new DecompressionStream('deflate-raw');
  const stream = new Blob([bytes as BlobPart]).stream().pipeThrough(ds);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/**
 * Read a zip. Walks the central directory, so an archive with a comment or
 * extra fields opens; stored and deflated entries both come back as bytes.
 */
export async function readZip(buf: ArrayBuffer): Promise<ZipEntry[]> {
  const v = new DataView(buf);
  const u8 = new Uint8Array(buf);
  // find the end-of-central-directory record, scanning back past any comment
  let eocd = -1;
  for (let i = buf.byteLength - 22; i >= 0 && i > buf.byteLength - 22 - 65536; i--)
    if (v.getUint32(i, true) === 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) throw new Error('not a zip archive');
  const count = v.getUint16(eocd + 10, true);
  let p = v.getUint32(eocd + 16, true);
  const out: ZipEntry[] = [];
  const decoder = new TextDecoder();
  for (let n = 0; n < count; n++) {
    if (v.getUint32(p, true) !== 0x02014b50) throw new Error('damaged zip directory');
    const method = v.getUint16(p + 10, true);
    const compSize = v.getUint32(p + 20, true);
    const nameLen = v.getUint16(p + 28, true);
    const extraLen = v.getUint16(p + 30, true);
    const commentLen = v.getUint16(p + 32, true);
    const local = v.getUint32(p + 42, true);
    const name = decoder.decode(u8.subarray(p + 46, p + 46 + nameLen));
    // the local header repeats the name and may carry a different extra field
    const lNameLen = v.getUint16(local + 26, true);
    const lExtraLen = v.getUint16(local + 28, true);
    const start = local + 30 + lNameLen + lExtraLen;
    const raw = u8.subarray(start, start + compSize);
    if (!name.endsWith('/'))
      out.push({ name, data: method === 0 ? raw : await inflateRaw(raw) });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return out;
}

/** Hand a blob to the browser as a download. */
export function saveAs(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
