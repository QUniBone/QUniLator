// Query-parameter helper on top of preact-iso. Path changes go through
// loc.route(path) (pushState); ephemeral query changes use loc.route(url, true)
// (replaceState), so back/forward step between meaningful screens only.
import { useLocation } from 'preact-iso';

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
