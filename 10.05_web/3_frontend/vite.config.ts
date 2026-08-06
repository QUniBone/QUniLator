import { defineConfig } from 'vite';
import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

// The dev server talks to a live QUniLator. Access is HTTP basic auth with the
// operator's name and password — the name in QBONE_USER or ~/.qbone-user, the
// password in ~/.qbone-pw — forwarded on the proxied /api and /ws requests so
// the app runs against real data while developing.
function boardAuth(): string {
  try {
    const pw = readFileSync(join(homedir(), '.qbone-pw'), 'utf8').trim();
    const user = (
      process.env.QBONE_USER ?? readFileSync(join(homedir(), '.qbone-user'), 'utf8')
    ).trim();
    return 'Basic ' + Buffer.from(user + ':' + pw).toString('base64');
  } catch {
    return '';
  }
}

const AUTH = boardAuth();
const authHeader: Record<string, string> = AUTH ? { Authorization: AUTH } : {};
const BOARD_HTTP = 'http://qbone';
const BOARD_WS = 'ws://qbone';

// The version this bundle is built from, read from the same
// packaging/debian/changelog the package and the binary take theirs from. The
// page compares it with what the server reports and reloads when they differ,
// which is what carries an open page onto the matching bundle after an update.
// Resolved against this file, so the working directory does not matter.
function bundleVersion(): string {
  try {
    const changelog = readFileSync(
      new URL('../../packaging/debian/changelog', import.meta.url),
      'utf8'
    );
    const first = changelog.split('\n')[0].match(/^[a-z]+ \(([^)]*)\)/);
    if (first) return first[1];
  } catch {
    /* fall through */
  }
  return '0.0.0-dev';
}

export default defineConfig(({ command }) => ({
  base: '/',
  define: {
    __QUNILATOR_VERSION__: JSON.stringify(bundleVersion()),
    // The dev server serves a bundle built from this checkout while the board it
    // proxies to runs its own version, so the page must not treat that
    // difference as a service it should reload onto.
    __QUNILATOR_DEV__: JSON.stringify(command === 'serve'),
  },
  build: {
    outDir: 'dist',
    // fail the build on anything worth a second look; the packaging expects
    // a clean, warning-free bundle
    chunkSizeWarningLimit: 2000,
  },
  server: {
    proxy: {
      '/api': { target: BOARD_HTTP, changeOrigin: true, headers: authHeader },
      '/ws': { target: BOARD_WS, ws: true, changeOrigin: true, headers: authHeader },
    },
  },
}));
