// The version this bundle was built from, substituted by vite.config.ts out of
// packaging/debian/changelog. The page compares it with GET /api/version.
declare const __QUNILATOR_VERSION__: string;

// True in a `vite dev` bundle, which is built from the checkout's changelog and
// proxies to a board running its own version.
declare const __QUNILATOR_DEV__: boolean;

// Minimal Web Serial typings — the console's "Web Serial" source. The
// full spec is not in lib.dom; only the members the console uses are declared.
interface WebSerialPort {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
}
interface WebSerial {
  requestPort(): Promise<WebSerialPort>;
  addEventListener(
    type: 'disconnect',
    listener: (event: { target: WebSerialPort }) => void
  ): void;
}
interface Navigator {
  readonly serial?: WebSerial;
}
