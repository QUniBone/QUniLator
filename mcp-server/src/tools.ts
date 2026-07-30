/* tools.ts: the named MCP tools, each a thin wrapper over one QBone REST call
 * or one short WS subscription. Handlers return the board's JSON as text so an
 * agent sees exactly what the API returned.
 */
import { z } from "zod";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { QBoneClient, QBoneError, LOG_LEVELS } from "./qbone.js";
import type { ConsoleChannel, LogLevelName } from "./qbone.js";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

/* The directory of the XXDP 2.5 RL02 pack, read off the image once by
   tools/xxdp-dir.py and shipped here. Reading it from the pack itself means
   booting XXDP and waiting several minutes for a console listing to scroll
   past, which is not worth doing to find out what a diagnostic is called. */
type XxdpFile = { name: string; date: string; blocks: number };

let xxdpPack: XxdpFile[] | null = null;

function xxdpPackFiles(): XxdpFile[] {
  if (xxdpPack === null) {
    const here = dirname(fileURLToPath(import.meta.url));
    // src/ when running from source, dist/src/ when built
    const candidates = [
      join(here, "..", "data", "xxdp25-rl02.json"),
      join(here, "..", "..", "data", "xxdp25-rl02.json"),
    ];
    let last: unknown;
    for (const path of candidates) {
      try {
        xxdpPack = JSON.parse(readFileSync(path, "utf8")) as XxdpFile[];
        return xxdpPack;
      } catch (err) {
        last = err;
      }
    }
    throw new Error(
      `xxdp25-rl02.json not found: ${last instanceof Error ? last.message : String(last)}`,
    );
  }
  return xxdpPack;
}

type ToolResult = {
  content: { type: "text"; text: string }[];
  isError?: boolean;
};

function ok(value: unknown): ToolResult {
  const text =
    typeof value === "string" ? value : JSON.stringify(value, null, 2);
  return { content: [{ type: "text", text }] };
}

function fail(err: unknown): ToolResult {
  const msg =
    err instanceof QBoneError
      ? `QBone error ${err.status}: ${err.message}`
      : err instanceof Error
        ? err.message
        : String(err);
  return { content: [{ type: "text", text: msg }], isError: true };
}

async function run(fn: () => Promise<unknown>): Promise<ToolResult> {
  try {
    return ok(await fn());
  } catch (err) {
    return fail(err);
  }
}

const channelSchema = z
  .enum(["0", "1", "ext", "vax"])
  .describe(
    "console channel: 0 = DL11 @777560, 1 = @776500, ext = /dev/ttyS2, " +
      "vax = the emulated VAX-11/780's own console",
  );

export function registerTools(server: McpServer, qbone: QBoneClient): void {
  // ---- Observation ------------------------------------------------------

  server.registerTool(
    "get_log",
    {
      description:
        "Tail the QBone log stream (/ws/events) for a window and return the " +
        "lines at or above a severity (each line: label, level, text). The " +
        "board keeps no server-side log history, so this collects what is " +
        "emitted during the window. IMPORTANT: every source defaults to " +
        "'warning', so debug/info lines appear ONLY after you raise that " +
        "source with set_log_level (e.g. set_log_level('tqk50','debug')) — " +
        "raise it first, then get_log with a matching level.",
      inputSchema: {
        level: z
          .enum(["fatal", "error", "warning", "info", "debug"])
          .default("info")
          .describe("minimum severity to include (fatal is most severe)"),
        duration_ms: z.number().int().min(100).max(60000).default(2000),
        max_lines: z.number().int().min(1).max(5000).default(500),
      },
    },
    async ({ level, duration_ms, max_lines }) =>
      run(() =>
        qbone.getLog(level as LogLevelName, duration_ms, max_lines),
      ),
  );

  server.registerTool(
    "get_logging",
    {
      description:
        "List the log sources and their current levels (GET /api/logging), " +
        "plus the default level. Use to find a source's label before raising it.",
      inputSchema: {},
    },
    async () => run(() => qbone.getLogging()),
  );

  server.registerTool(
    "set_log_level",
    {
      description:
        "Raise or lower a log source's severity (PUT /api/logging/sources/" +
        "<source>). Sources default to 'warning'; set the one you want to " +
        "'debug' BEFORE capturing with get_log, then set it back to 'warning' " +
        "when done so the stream stays quiet. Source is the label from " +
        "get_logging / get_devices (e.g. 'tqk50', 'mscp_server').",
      inputSchema: {
        source: z.string(),
        level: z.enum(["fatal", "error", "warning", "info", "debug"]),
      },
    },
    async ({ source, level }) =>
      run(() => qbone.setLogLevel(source, level)),
  );

  server.registerTool(
    "console_read",
    {
      description:
        "Snapshot a console channel's current buffer: connect the channel " +
        "WebSocket, which replays its retained ring, collect the replay, and " +
        "return it as text.",
      inputSchema: {
        channel: channelSchema,
        settle_ms: z.number().int().min(50).max(5000).default(300),
        timeout_ms: z.number().int().min(200).max(30000).default(2000),
      },
    },
    async ({ channel, settle_ms, timeout_ms }) =>
      run(async () => ({
        channel,
        output: await qbone.consoleRead(channel as ConsoleChannel, {
          settleMs: settle_ms,
          timeoutMs: timeout_ms,
        }),
      })),
  );

  server.registerTool(
    "console_send",
    {
      description:
        "Send input to a console channel. The text is sent as raw bytes; set " +
        "append_cr to append a carriage return.",
      inputSchema: {
        channel: channelSchema,
        text: z.string().describe("bytes to send (interpreted as latin1)"),
        append_cr: z.boolean().default(false),
      },
    },
    async ({ channel, text, append_cr }) =>
      run(async () => {
        const payload = append_cr ? text + "\r" : text;
        await qbone.consoleSend(
          channel as ConsoleChannel,
          Buffer.from(payload, "latin1"),
        );
        return { channel, sent: payload.length };
      }),
  );

  server.registerTool(
    "get_devices",
    {
      description:
        "The device set with parameters, the friendly label, and the backend " +
        "status field (off/idle/loaded/ready/busy) returned as-is, from " +
        "GET /api/devices.",
      inputSchema: {},
    },
    async () => run(() => qbone.getDevices()),
  );

  server.registerTool(
    "get_machine_state",
    {
      description:
        "Front-panel/bus view from the /ws/events state snapshot. Returns a " +
        "derived `running` (CPU executing = powered && HALT released) plus the " +
        "raw halt, powered, activity leds[] and DIP switches[] (SW0 = LSB). " +
        "ALWAYS check `running` before trying to boot or run anything: a " +
        "running CPU is the prerequisite. `running:false` with powered means " +
        "the CPU is halted (in micro-ODT `@`) — use start_machine to bring it up.",
      inputSchema: {},
    },
    async () => run(() => qbone.getMachineState()),
  );

  // ---- Control ----------------------------------------------------------

  server.registerTool(
    "set_param",
    {
      description:
        "Write a device parameter (PUT /api/devices/<dev>/params/<param>). " +
        "Attaching a disk image is a write to the drive's image parameter; an " +
        "empty value detaches.",
      inputSchema: {
        device: z.string(),
        param: z.string().describe("full name or short name"),
        value: z.string(),
      },
    },
    async ({ device, param, value }) =>
      run(() => qbone.setParam(device, param, value)),
  );

  server.registerTool(
    "set_device_enabled",
    {
      description:
        "Enable or disable a device (the enabled parameter). Disabling a " +
        "controller disables the drives it contains.",
      inputSchema: {
        device: z.string(),
        enabled: z.boolean(),
      },
    },
    async ({ device, enabled }) =>
      run(() => qbone.setParam(device, "enabled", enabled ? "true" : "false")),
  );

  server.registerTool(
    "start_machine",
    {
      description:
        "Bring the machine up RUNNING and confirm it (the reliable way to begin " +
        "a test run). Issues restart (release HALT, then restart the CPU from " +
        "the power-up vector) — or dc_on (logical power-on running) — then waits " +
        "for a /ws/events frame reporting the CPU running, and returns that " +
        "state. The boot ROM then auto-boots the first bootable device; there is " +
        "NO boot dialog to drive. Use this instead of powercycle after a halt: " +
        "powercycle does not release HALT, so a halted CPU comes up in ODT. " +
        "Note: restart keeps the currently-applied config, while dc_on re-runs " +
        "the DIP config selection (see control).",
      inputSchema: {
        action: z
          .enum(["restart", "dc_on"])
          .optional()
          .describe("default restart; dc_on also re-runs DIP config selection"),
        timeout_ms: z.number().int().positive().optional(),
      },
    },
    async ({ action, timeout_ms }) =>
      run(() => qbone.powerUp(action ?? "restart", timeout_ms ?? 8000)),
  );

  server.registerTool(
    "wait_for_running",
    {
      description:
        "Block until the CPU is running (powered with HALT released) or timeout. " +
        "The opening /ws/events snapshot counts, so an already-running machine " +
        "returns at once. Counterpart to wait_for_halt.",
      inputSchema: { timeout_ms: z.number().int().positive() },
    },
    async ({ timeout_ms }) => run(() => qbone.waitForRunning(timeout_ms)),
  );

  server.registerTool(
    "list_xxdp_diagnostics",
    {
      description:
        "List what is on the XXDP 2.5 RL02 pack, so a diagnostic's name does " +
        "not have to be found by booting XXDP and listing the pack over the " +
        "console. Answers from a directory read off the image, not from the " +
        "board, so it needs no machine and costs nothing. `match` is a regular " +
        "expression against the file name, case insensitive: 'ZTK' for the " +
        "TK50 diagnostics, 'ZRL' for the RL, 'ZUD' for the UDA50, 'ZRQ' for " +
        "the RQDX, 'ZRX' for the RX. Names here carry the extension and drop " +
        "the leading class letter that the DEC part number has (CZTKAE0 is " +
        "ZTKAE0.BIN), which is the name run_xxdp_diagnostic wants.",
      inputSchema: {
        match: z
          .string()
          .optional()
          .describe("regular expression on the file name, e.g. '^ZTK'"),
        runnable_only: z
          .boolean()
          .optional()
          .describe("only .BIC and .BIN, the loadable programs (default true)"),
        limit: z.number().int().positive().optional().describe("default 200"),
      },
    },
    async ({ match, runnable_only, limit }) =>
      run(async () => {
        let re: RegExp | null = null;
        if (match !== undefined) {
          try {
            re = new RegExp(match, "i");
          } catch (err) {
            throw new Error(
              `invalid match: ${err instanceof Error ? err.message : String(err)}`,
            );
          }
        }
        const runnable = runnable_only ?? true;
        const all = xxdpPackFiles().filter(
          (f) =>
            (!runnable || f.name.endsWith(".BIC") || f.name.endsWith(".BIN")) &&
            (re === null || re.test(f.name)),
        );
        const max = limit ?? 200;
        return {
          pack: "xxdp25.rl02",
          matched: all.length,
          truncated: all.length > max,
          files: all.slice(0, max),
        };
      }),
  );

  server.registerTool(
    "run_xxdp_diagnostic",
    {
      description:
        "Run an XXDP diagnostic end-to-end and report pass/fail. Applies a " +
        "config, applies device setup, power-cycles the machine up running " +
        "(the boot ROM auto-boots XXDP), then over the console loads the " +
        "diagnostic, drives the DRS Change-HW dialog from `answers`, runs it, " +
        "and returns {passed, terminatedBy, transcript}. `diagnostic` is the " +
        "XXDP filename with the leading class letter dropped (CZTKAE0 -> " +
        "ZTKAE0). Each answer sends `value` when a prompt contains `match` " +
        "(value \"\" accepts the prompt default). Typical for the TK50: " +
        "config 'xxdp'; setup enables tqk50 and tqk500 (a scratch .tap image); " +
        "diagnostic 'ZTKAE0'; answers [{match:'CHANGE HW',value:'Y'}, " +
        "{match:'UNITS',value:'1'}, {match:'ADDRESS',value:''}, " +
        "{match:'VECTOR',value:''}, {match:'UNIT NUMBER',value:''}].",
      inputSchema: {
        diagnostic: z.string().describe("XXDP file, e.g. ZTKAE0"),
        config: z.string().optional().describe("config to apply first, e.g. xxdp"),
        console: channelSchema
          .optional()
          .describe(
            "console the machine talks on: 'ext' (a CPU with its own SLU, the " +
              "default) or '0' (an emulated CPU's emulated DL11)",
          ),
        setup: z
          .array(
            z.object({
              device: z.string(),
              param: z.string().optional(),
              value: z.string().optional(),
              enabled: z.boolean().optional(),
            }),
          )
          .optional()
          .describe("device edits: {device,enabled} or {device,param,value}"),
        answers: z
          .array(z.object({ match: z.string(), value: z.string() }))
          .optional()
          .describe("DRS dialog: send value when a prompt contains match"),
        pass_pattern: z.string().optional().describe("default 'END PASS'"),
        run_timeout_ms: z.number().int().positive().optional(),
      },
    },
    async ({
      diagnostic,
      config,
      console: channel,
      setup,
      answers,
      pass_pattern,
      run_timeout_ms,
    }) =>
      run(() =>
        qbone.runXxdpDiagnostic({
          diagnostic,
          config,
          console: channel as ConsoleChannel | undefined,
          setup,
          answers,
          passPattern: pass_pattern,
          runTimeoutMs: run_timeout_ms,
        }),
      ),
  );

  server.registerTool(
    "control",
    {
      description:
        "Low-level bus/power control (POST /api/control). Prefer start_machine " +
        "to come up running. Actions: restart = release HALT then restart the " +
        "CPU running from the power-up vector (keeps the applied config); " +
        "dc_on = logical power-on running; dc_off = halt + clear powered; " +
        "powercycle = DCOK/POK cycle that does NOT release HALT (a halted CPU " +
        "comes up in micro-ODT) AND re-runs the DIP->config selection, RE-" +
        "APPLYING that config and discarding runtime device edits (an enabled " +
        "device, a swapped image); init = pulse BINIT. powercycle/dc_on/dc_off " +
        "disrupt a running machine.",
      inputSchema: {
        action: z.enum(["powercycle", "init", "restart", "dc_on", "dc_off"]),
      },
    },
    async ({ action }) => run(() => qbone.control(action)),
  );

  server.registerTool(
    "halt",
    {
      description:
        "Assert the QBUS HALT line (POST /api/control halt); the CPU stops. To " +
        "resume from where it stopped use continue; to reboot running use " +
        "start_machine (NOT powercycle, which comes up halted in ODT after a halt).",
      inputSchema: {},
    },
    async () => run(() => qbone.control("halt")),
  );

  server.registerTool(
    "continue",
    {
      description: "Release HALT and resume the CPU (POST /api/control continue).",
      inputSchema: {},
    },
    async () => run(() => qbone.control("continue")),
  );

  server.registerTool(
    "configs",
    {
      description:
        "Manage named configurations. list: GET /api/configs. apply/switch: " +
        "POST /api/configs/<name>/apply (makes <name> current). save: capture " +
        "the live setup under <name>. set_default: PUT " +
        "/api/configs/<name>/default.",
      inputSchema: {
        action: z.enum(["list", "apply", "switch", "save", "set_default"]),
        name: z
          .string()
          .optional()
          .describe("required for apply/switch/save/set_default"),
      },
    },
    async ({ action, name }) =>
      run(async () => {
        if (action === "list") return qbone.getConfigs();
        if (!name) throw new Error(`action "${action}" requires a name`);
        if (action === "apply" || action === "switch")
          return qbone.applyConfig(name);
        if (action === "set_default") return qbone.setDefaultConfig(name);
        // save: fetch the live setup, then store it under <name>
        const live = await qbone.getLiveConfig();
        return qbone.saveLiveConfig(name, live);
      }),
  );

  server.registerTool(
    "images",
    {
      description:
        "Manage disk images. list: GET /api/images. upload: POST a local file " +
        "(path on the workstation) to /api/images. attach: point a removable " +
        "drive's image parameter at an image (empty image detaches).",
      inputSchema: {
        action: z.enum(["list", "upload", "attach"]),
        path: z
          .string()
          .optional()
          .describe("upload: local file path on the workstation"),
        name: z
          .string()
          .optional()
          .describe("upload: image name to store as (defaults to the file name)"),
        device: z.string().optional().describe("attach: target drive"),
        image: z
          .string()
          .optional()
          .describe("attach: image name, or empty string to detach"),
      },
    },
    async ({ action, path, name, device, image }) =>
      run(async () => {
        if (action === "list") return qbone.getImages();
        if (action === "upload") {
          if (!path) throw new Error("upload requires a path");
          const data = readFileSync(path);
          const imageName = name ?? path.split("/").pop() ?? "image";
          return qbone.uploadImage(imageName, data);
        }
        // attach
        if (!device) throw new Error("attach requires a device");
        return qbone.setParam(device, "image", image ?? "");
      }),
  );

  // ---- Wait-for helpers -------------------------------------------------

  server.registerTool(
    "wait_for_halt",
    {
      description:
        "Hold a /ws/events subscription and resolve when a state event reports " +
        "halt (an already-halted machine resolves at once), or at timeout.",
      inputSchema: {
        timeout_ms: z.number().int().min(100).max(600000).default(30000),
      },
    },
    async ({ timeout_ms }) => run(() => qbone.waitForHalt(timeout_ms)),
  );

  server.registerTool(
    "wait_for_console",
    {
      description:
        "Connect /ws/console/<channel> (which replays its ring then streams " +
        "live) and resolve when the accumulated output matches the pattern, or " +
        "at timeout. A pattern that already scrolled past is still caught " +
        "within the retained window.",
      inputSchema: {
        channel: channelSchema,
        pattern: z.string().describe("JavaScript regular expression source"),
        timeout_ms: z.number().int().min(100).max(600000).default(30000),
      },
    },
    async ({ channel, pattern, timeout_ms }) =>
      run(async () => {
        let re: RegExp;
        try {
          re = new RegExp(pattern);
        } catch (err) {
          throw new Error(
            `invalid pattern: ${err instanceof Error ? err.message : String(err)}`,
          );
        }
        return qbone.waitForConsole(channel as ConsoleChannel, re, timeout_ms);
      }),
  );
}

// Re-export so the entry point and tests can reference the level set.
export { LOG_LEVELS };
