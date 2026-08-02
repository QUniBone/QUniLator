import './styles.css';
import { render } from 'preact';
import { LocationProvider } from 'preact-iso';
import { html } from './html';
import { App } from './components/Shell';
import { setStore } from './store';
import { checkAuth } from './lib/modals';
import { refreshDevices, refreshSettings } from './api';
import { initEvents, eventsWs } from './lib/events';
import { syncVersion } from './lib/version';
import { resumeInstallIfPending } from './lib/update';
import { shutdownTerminals } from './lib/terminals';
import { vcb01Socket } from './lib/vcb01';

// name the tab after the board it is serving, e.g. "qbone - QUniLator"
document.title = location.hostname ? location.hostname + ' - QUniLator' : 'QUniLator';

// preact-iso's LocationProvider claims every click on a same-origin <a> and turns
// it into a route change. A download anchor is same-origin too - both the API's
// image links and the blob: URL an export hands to the browser - so without this
// the click is swallowed and history.pushState() is asked to store a blob: URL,
// which throws a SecurityError and leaves the user with no file. Stopping the
// event in the capture phase keeps it from reaching the router's window listener
// while leaving the browser's own download behaviour intact.
document.addEventListener(
  'click',
  (e) => {
    const t = e.target as Element | null;
    if (t && typeof t.closest === 'function' && t.closest('a[download]')) e.stopPropagation();
  },
  true
);

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
    await syncVersion().catch(() => {});
    await refreshDevices();
    setStore({ connected: true });
    await refreshSettings().catch(() => {});
    initEvents();
  } catch {
    setStore({ connected: false });
  }
}

// A page loaded or reloaded while an install was under way picks the overlay back
// up, rather than showing whatever the half-replaced server happened to answer.
// Before initLive, and independent of it: the point is that the server may not be
// answering at all.
resumeInstallIfPending();
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
