// htm bound to Preact's hyperscript — the same template syntax the original
// single-file app used, so components port verbatim.
import { h } from 'preact';
import htm from 'htm';

export const html = htm.bind(h);
