// Usage: node smoke.mjs --project <demo.uproject> --editor <UnrealEditor-Cmd.exe>
import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const { values } = parseArgs({ options: { project: { type: "string" }, editor: { type: "string" } } });
const root = path.join(path.dirname(path.resolve(values.project)), "Content", "AngelIO");
const client = new Client({ name: "angelio-smoke", version: "1" });
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [fileURLToPath(new URL("./server.mjs", import.meta.url)), ...process.argv.slice(2)]
});
const call = (name, args = {}) => client.callTool({ name, arguments: args }, undefined, { timeout: 150000 });

let fixture;
try {
  await client.connect(transport);
  const tools = (await client.listTools()).tools.map(tool => tool.name);
  assert.deepEqual(tools.sort(), ["describe_api", "list_scripts", "read_script", "validate_script"]);
  assert((await call("list_scripts")).structuredContent.scripts.includes("Elevator.as"));
  const api = (await call("describe_api")).structuredContent;
  assert.equal(api.entry, "void Run()");
  assert(api.externalDeclarations.some(s => s.includes("Lift::MoveTo(int floor, float speed)")));
  assert(api.externalDeclarations.some(s => s.includes("LiftTrip::Wait()")));
  assert((await call("validate_script", { script: "TypedLift.as" })).structuredContent.valid);
  assert(api.calls.some(signature => signature.startsWith("SendTo(")));
  const panel = (await call("validate_script", { script: "LiftPanel.as" })).structuredContent;
  assert(panel.valid && panel.inputSignals.includes("SelectFloor"));
  assert((await call("read_script", { script: "Elevator.as" })).structuredContent.source.includes("void Run()"));
  const good = (await call("validate_script", { script: "Elevator.as" })).structuredContent;
  assert(good.valid);
  assert(good.ports.some(p => p.name === "TravelSpeed" && p.type === "float" && p.direction === "in"));
  assert(good.ports.some(p => p.name === "LastFloor" && p.type === "int" && p.direction === "out"));
  assert(good.outputSignals.includes("Arrived"));

  fixture = await mkdtemp(path.join(root, "MCP smoke "));
  const invalid = path.join(fixture, "Broken mission.as");
  await writeFile(invalid, "void Run() {\n    UnknownCommand();\n}\n");
  const response = await call("validate_script", { script: path.relative(root, invalid) });
  assert(!response.isError, JSON.stringify(response.content));
  const bad = response.structuredContent;
  assert.equal(bad.valid, false);
  assert(bad.diagnostics.some(d => d.line === 2 && d.column > 0 && d.severity === "error"));
  assert((await call("read_script", { script: "../Outside.as" })).isError);
  console.log("MCP stdio OK: discovery, source, API, valid contract, compiler error location, path scope.");
} finally {
  if (fixture) await rm(fixture, { recursive: true, force: true });
  await client.close();
}
