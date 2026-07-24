// Minimal Web Serial typings — the console's "Mac (Web Serial)" source. The
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
