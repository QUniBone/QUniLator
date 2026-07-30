// The version the board runs, against the version this bundle was built from.
//
// The bundle and the binary are built from one changelog, so a difference means
// the service has been replaced under an open page — through the interface, or
// by an apt upgrade over ssh. The page then reloads and fetches the matching
// bundle: civetweb serves the docroot with static_file_max_age 0, so the reload
// revalidates rather than serving the old file from cache.
import { store, setStore } from '../store';

export interface VersionInfo {
  package: string;
  version: string;
  built: string;
}

// The version this bundle carries, substituted at build time from
// packaging/debian/changelog.
export const bundleVersion: string = __QUNILATOR_VERSION__;

export async function fetchVersion(): Promise<VersionInfo | null> {
  try {
    const r = await fetch('/api/version', { cache: 'no-store' });
    if (!r.ok) return null;
    const v = (await r.json()) as VersionInfo;
    if (!v || typeof v.version !== 'string') return null;
    return v;
  } catch {
    return null;
  }
}

// One reload per server version. A bundle and a service that disagree for a
// reason a reload cannot fix — a development build served from the dev server,
// a docroot that was not replaced with the binary — would otherwise reload the
// page forever.
const RELOADED_KEY = 'qunilator.reloadedFor';

// Read the server's version into the store and reload when it is not the one
// this bundle was built from. Called at startup and on every /ws/events
// reconnect, so the page follows the service across a restart.
export async function syncVersion(): Promise<void> {
  const v = await fetchVersion();
  if (v == null) return;
  if (v.version !== store.serverVersion || v.package !== store.serverPackage)
    setStore({ serverVersion: v.version, serverPackage: v.package, serverBuilt: v.built });
  if (v.version === bundleVersion) {
    sessionStorage.removeItem(RELOADED_KEY);
    return;
  }
  // The dev server builds the bundle from this checkout's changelog while the
  // board runs whatever it runs; reloading would not bring the two together.
  if (__QUNILATOR_DEV__) return;
  if (sessionStorage.getItem(RELOADED_KEY) === v.version) return;
  sessionStorage.setItem(RELOADED_KEY, v.version);
  location.reload();
}
