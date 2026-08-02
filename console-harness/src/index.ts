/* Public API of the harness as a library. */
export {
  type Transport,
  WsTransport,
  TcpTransport,
  type WsTransportOptions,
} from "./transport.js";
export {
  Session,
  SessionError,
  ExpectTimeoutError,
  DeviationError,
  EchoStallError,
  DEFAULT_TUNING,
  type SessionOptions,
  type Deviation,
  type InputMode,
  type InputTuning,
  type ExpectOutcome,
  type FailureDiagnostics,
} from "./session.js";
export {
  MachineEvents,
  type EventSource,
  type MachineEventName,
} from "./events.js";
export {
  CastRecorder,
  readCast,
  readNotes,
  type Cast,
  type CastEvent,
  type CastHeader,
  type CastNotes,
  type EchoSpan,
  type StepRecord,
} from "./recording.js";
export {
  loadScript,
  validateScript,
  runScript,
  parseDuration,
  interpolate,
  ScriptFailure,
  type ScriptSpec,
  type StepSpec,
  type CaseSpec,
  type RunResult,
} from "./steps.js";
export {
  makeTarget,
  loadPassword,
  basicAuth,
  resolveConsoleChannel,
  consoleWsUrl,
  eventsWsUrl,
  type BoardTarget,
} from "./board.js";
export { compilePattern, escapeRegExp, scan } from "./matcher.js";
