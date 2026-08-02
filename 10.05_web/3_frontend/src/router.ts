// Query-parameter helper on top of preact-iso. Path changes go through
// loc.route(path) (pushState); ephemeral query changes use loc.route(url, true)
// (replaceState), so back/forward step between meaningful screens only.
import { useLocation } from 'preact-iso';

/**
 * Let a download link reach the browser.
 *
 * LocationProvider turns every same-origin anchor click into a route change:
 * it cancels the click and pushes the href. A link that carries a file is not
 * navigation — the export menu clicks an `<a download>` on a `blob:` URL, and
 * the storage screen links straight at `/api/images/…` — so routing one both
 * loses the download and, for a `blob:` URL, throws SecurityError out of
 * pushState. Claim those clicks in the capture phase, which reaches the window
 * before the provider's bubble-phase listener, and let the browser have them.
 */
export function installDownloadClickGuard(): void {
  addEventListener(
    'click',
    (ev) => {
      const target = ev.target as HTMLElement | null;
      const a = target?.closest?.('a[href]') as HTMLAnchorElement | null;
      if (a && (a.hasAttribute('download') || !/^https?:$/.test(a.protocol)))
        ev.stopPropagation();
    },
    true
  );
}

export function useQueryParam(key: string): [string, (v: string) => void] {
  const loc = useLocation();
  const query = (loc.query as Record<string, string>) || {};
  const value = query[key] || '';
  const set = (v: string) => {
    const params = new URLSearchParams(query);
    if (v) params.set(key, v);
    else params.delete(key);
    const qs = params.toString();
    loc.route(loc.path + (qs ? '?' + qs : ''), true);
  };
  return [value, set];
}
