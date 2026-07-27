// Serial-line endpoint translation — pure UI logic, no backend evaluation.
//
// A TCP serial line is stored on the backend as three parameters: a role
// (`listen` or `connect`), a host, and a port. In the configuration editor an
// operator manages the line as ONE field, and this module is the only place
// that knows how that single string maps to and from the three parameters:
//
//   "4000"       -> { role: 'listen',  host: '',    port: 4000 }  (bare port)
//   ":4000"      -> { role: 'listen',  host: '',    port: 4000 }
//   "box:4000"   -> { role: 'connect', host: 'box', port: 4000 }
//   "ws"         -> { role: 'websocket', host: '', port: 0     }  (browser terminal)
//   ""           -> { role: '',        host: '',    port: 0    }  (line off)
//
// format() is the inverse: it renders an endpoint back to the canonical field
// text. It never talks to the network; it only shapes strings.

export type SerialRole = 'listen' | 'connect' | 'websocket' | '';

export interface SerialEndpointValue {
  role: SerialRole;
  host: string;
  port: number;
}

function parse(text: string): SerialEndpointValue {
  const s = (text || '').trim();
  if (!s) return { role: '', host: '', port: 0 };
  // a browser terminal over WebSocket needs no host or port
  if (s.toLowerCase() === 'ws' || s.toLowerCase() === 'websocket')
    return { role: 'websocket', host: '', port: 0 };
  const colon = s.lastIndexOf(':');
  if (colon < 0) {
    // a bare number listens on that port; anything else is not a port
    const port = parseInt(s, 10);
    return Number.isFinite(port) && port > 0
      ? { role: 'listen', host: '', port }
      : { role: '', host: '', port: 0 };
  }
  const host = s.slice(0, colon).trim();
  const port = parseInt(s.slice(colon + 1), 10) || 0;
  // a host before the colon connects out; a leading colon listens
  return host ? { role: 'connect', host, port } : { role: 'listen', host: '', port };
}

function format(ep: SerialEndpointValue): string {
  if (!ep) return '';
  if (ep.role === 'websocket') return 'ws';
  if (!ep.port) return '';
  if (ep.role === 'connect' && ep.host) return ep.host + ':' + ep.port;
  return String(ep.port);
}

export const serialEndpoint = { parse, format };

// ---- which lines a serial device exposes ----
// The three parameters name the line by a trailing index (`tcp_role0`,
// `tcp_host0`, `tcp_port0`, …) on a multiplexer, or carry no index at all on a
// single-line DL11 (`tcp_role`, `tcp_host`, `tcp_port`). A line is only listed
// when its role parameter is actually present, so a device whose per-line
// parameters the backend has not yet grown simply shows no serial field.

export interface SerialLine {
  index: number | null; // null on a single unindexed line (DL11)
  label: string;
  roleParam: string;
  hostParam: string;
  portParam: string;
}

export function serialLines(params: { n: string }[]): SerialLine[] {
  const has = (n: string) => params.some((p) => p.n === n);
  if (has('tcp_role'))
    return [{ index: null, label: 'Line', roleParam: 'tcp_role', hostParam: 'tcp_host', portParam: 'tcp_port' }];
  const lines: SerialLine[] = [];
  for (let i = 0; has('tcp_role' + i); i++)
    lines.push({
      index: i,
      label: 'Line ' + i,
      roleParam: 'tcp_role' + i,
      hostParam: 'tcp_host' + i,
      portParam: 'tcp_port' + i,
    });
  return lines;
}
