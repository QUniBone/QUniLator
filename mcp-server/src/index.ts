#!/usr/bin/env node
/* index.ts: the MCP server entry point. Loads the board config, builds the
 * QBone client, registers the tools, and serves them over stdio for an MCP
 * client to spawn.
 */
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { loadConfig } from "./config.js";
import { QBoneClient } from "./qbone.js";
import { registerTools } from "./tools.js";

async function main(): Promise<void> {
  const cfg = loadConfig();
  const qbone = new QBoneClient(cfg);
  const server = new McpServer({
    name: "qbone-mcp-server",
    version: "0.1.0",
  });
  registerTools(server, qbone);
  // Diagnostics go to stderr; stdout carries the MCP protocol.
  process.stderr.write(`qbone-mcp-server: board ${cfg.host}\n`);
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  process.stderr.write(`qbone-mcp-server: fatal: ${String(err)}\n`);
  process.exit(1);
});
