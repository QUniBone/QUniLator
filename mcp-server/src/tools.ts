/* tools.ts: the named MCP tools, each a thin wrapper over one QBone REST call
 * or one short WS subscription. Handlers return the board's JSON as text so an
 * agent sees exactly what the API returned.
 */
import { z } from "zod";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { QBoneClient, QBoneError, LOG_LEVELS } from "./qbone.js";
import type { ConsoleChannel, LogLevelName } from "./qbone.js";
import { readFileSync } from "node:fs";

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
  .enum(["0", "1", "ext"])
  .describe("console channel: 0 = DL11 @777560, 1 = @776500, ext = /dev/ttyS2");

export function registerTools(server: McpServer, qbone: QBoneClient): void {
  // ---- Observation ------------------------------------------------------

  server.registerTool(
    "get_log",
    {
      description:
        "Tail the QBone log stream (/ws/events) for a window and return the " +
        "lines at or above a severity. The board keeps no server-side log " +
        "history, so this collects what is emitted during the window.",
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
        "Front-panel/bus view from the /ws/events state snapshot: halt, " +
        "powered, activity leds[], and DIP switches[].",
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
    "control",
    {
      description:
        "Bus/power control (POST /api/control). dc_off/powercycle are " +
        "destructive to a running machine.",
      inputSchema: {
        action: z.enum(["powercycle", "init", "restart", "dc_on", "dc_off"]),
      },
    },
    async ({ action }) => run(() => qbone.control(action)),
  );

  server.registerTool(
    "halt",
    {
      description: "Assert the QBUS HALT line (POST /api/control halt).",
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
