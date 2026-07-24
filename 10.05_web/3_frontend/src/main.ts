import './styles.css';
import { render } from 'preact';
import { LocationProvider } from 'preact-iso';
import { html } from './html';
import { App } from './components/Shell';
import { setStore } from './store';
import { checkAuth } from './lib/modals';
import { refreshDevices, refreshSettings } from './api';
import { initEvents, eventsWs } from './lib/events';
import { shutdownTerminals } from './lib/terminals';
import { vcb01Socket } from './lib/vcb01';

render(
  html`<${LocationProvider}><${App} /></${LocationProvider}>`,
  document.getElementById('app')!
);

async function initLive(): Promise<void> {
  try {
    const st = await fetch('/api/state');
    if (!st.ok) throw new Error('state fetch failed');
    const state = await st.json();
    setStore({ imagesDir: state.images_dir || '', platform: state.platform || '' });
    await checkAuth().catch(() => {});
    await refreshDevices();
    setStore({ connected: true });
    await refreshSettings().catch(() => {});
    initEvents();
  } catch {
    setStore({ connected: false });
  }
}
initLive();

// Close every WebSocket when the page goes away, so a reload or navigation
// frees the server's per-socket worker thread at once rather than leaving it to
// the ping/pong timeout.
window.addEventListener('pagehide', () => {
  const shut = (ws: WebSocket | null) => {
    try {
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
    } catch {
      /* ignore */
    }
  };
  shut(eventsWs);
  shut(vcb01Socket());
  shutdownTerminals();
});
