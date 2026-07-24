// Imperative toast overlay, appended into the #toasts host in index.html.
import { esc } from './util';

export function toast(cmd: string, msg: string): void {
  const host = document.getElementById('toasts');
  if (!host) return;
  const el = document.createElement('div');
  el.className = 'toast';
  el.innerHTML =
    (cmd ? '<div class="cmd">&gt; ' + esc(cmd) + '</div>' : '') +
    (msg ? '<div class="msg">' + esc(msg) + '</div>' : '');
  host.appendChild(el);
  setTimeout(() => el.remove(), 3600);
}
