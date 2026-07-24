import { defineConfig } from 'vite';
import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

// The dev server talks to the live board. Board access is HTTP basic auth with
// any user name and the password in ~/.qbone-pw; forward it on the proxied
// /api and /ws requests so the app runs against real data while developing.
function boardAuth(): string {
  try {
    const pw = readFileSync(join(homedir(), '.qbone-pw'), 'utf8').trim();
    return 'Basic ' + Buffer.from(':' + pw).toString('base64');
  } catch {
    return '';
  }
}

const AUTH = boardAuth();
const authHeader: Record<string, string> = AUTH ? { Authorization: AUTH } : {};
const BOARD_HTTP = 'http://qbone';
const BOARD_WS = 'ws://qbone';

export default defineConfig({
  base: '/',
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
});
