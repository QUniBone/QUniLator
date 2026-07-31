// Which widget a device gets, and the dashboard's view of it: the card and the
// options it offers the operator. A widget's size is measured, not declared.
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../../types';
import { DeviceWidget } from './base';
import type { WidgetOption, WidgetOpts } from './base';
import { DiskWidget, Ra81Widget, RemovableDriveWidget, RlWidget } from './drives';
import { CpuWidget } from './cpu';
import { MemoryWidget } from './memory';
import { VaxCpuWidget } from './cpuvax';
import { DhWidget, DzWidget, NetworkWidget, Vcb01Widget } from './peripherals';

export type { Cells, WidgetOption, WidgetOpts } from './base';
export { DeviceWidget } from './base';

// A widget class, as the registry holds it: constructible from a device and its
// stored options, and carrying the option list it declares.
export interface WidgetClass {
  new (d: LiveDev, opts: WidgetOpts): DeviceWidget;
  options: WidgetOption[];
}

// A device is matched by model first, so a drive with a front panel of its own
// gets it, and falls back on the widget its category shares.
const WIDGET_MODELS: Record<string, WidgetClass> = {
  RL02: RlWidget,
  RL01: RlWidget,
  RA81: Ra81Widget,
  VCB01: Vcb01Widget,
  RX01: RemovableDriveWidget,
  RX02: RemovableDriveWidget,
  dzv11_c: DzWidget,
  dhv11_c: DhWidget,
  'PDP-11/20': CpuWidget,
  'PDP-11/34': CpuWidget,
  'VAX-11/780': VaxCpuWidget,
};
const WIDGET_CATEGORIES: Record<string, WidgetClass> = {
  disk: DiskWidget,
  network: NetworkWidget,
  tape: RemovableDriveWidget,
  memory: MemoryWidget,
};

export function widgetFor(d: LiveDev): WidgetClass | null {
  return WIDGET_MODELS[d.type] || WIDGET_CATEGORIES[d.category || 'other'] || null;
}

// The options a device's widget offers; empty for one with nothing to configure.
export function widgetOptions(d: LiveDev): WidgetOption[] {
  const W = widgetFor(d);
  return W ? W.options : [];
}

// Render one device widget, for the dashboard grid to place as a card. The
// widget is built for this render and its render() runs here, inside the
// component, so a widget's controls may hold hooks of their own.
export function WidgetCard({ d, opts }: { d: LiveDev; opts?: WidgetOpts }): ComponentChildren {
  const W = widgetFor(d);
  return W ? new W(d, opts || {}).render() : null;
}
