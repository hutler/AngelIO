// Usage: node smoke.mjs --project <demo.uproject> --editor <UnrealEditor-Cmd.exe>
import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const { values } = parseArgs({ options: {
  project: { type: "string" }, editor: { type: "string" }, trace: { type: "string" }
} });
const root = path.join(path.dirname(path.resolve(values.project)), "Content", "AngelIO");
const client = new Client({ name: "angelio-smoke", version: "1" });
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [fileURLToPath(new URL("./server.mjs", import.meta.url)), "--project", values.project, "--editor", values.editor]
});
const call = (name, args = {}) => client.callTool({ name, arguments: args }, undefined, { timeout: 150000 });

let fixture;
try {
  await client.connect(transport);
  const tools = (await client.listTools()).tools.map(tool => tool.name);
  assert.deepEqual(tools.sort(), ["describe_api", "list_scripts", "read_script", "read_trace", "validate_script"]);
  assert((await call("read_trace", { trace: "../DefaultEngine.ini" })).isError);
  const newest = await call("read_trace", { limit: 1 });
  if (newest.isError) assert.match(newest.content[0].text, /No AngelIO trace found/);
  else assert.equal(newest.structuredContent.events.length, 1);
  if (values.trace) {
    const read = async (args = {}) => {
      const reply = await call("read_trace", { trace: values.trace, ...args });
      assert(!reply.isError, JSON.stringify(reply.content));
      return reply.structuredContent;
    };
    const history = await read();
    assert.equal(history.netMode, "listenServer");
    assert.equal(history.droppedEvents, 0);
    const calls = history.events.filter(e => e.type === "call");
    assert.equal(new Set(calls.map(e => e.operationId)).size, 6);
    for (const [floor, status] of [[6, "Arrived"], [7, "Rejected"], [1, "Cancelled"]]) {
      const trip = calls.find(event => event.arguments.floor === floor);
      assert.equal(trip.caller.script, "TypedLift.as");
      assert.equal(trip.caller.controller, floor === 7 ? "local" : "remote");
      const filter = { operationId: trip.operationId };
      const first = await read({ ...filter, limit: 1 });
      assert.deepEqual(first.events, [trip]);
      assert.equal(first.nextAfterSequence, trip.sequence);
      const rest = await read({ ...filter, afterSequence: first.nextAfterSequence });
      assert.equal(rest.nextAfterSequence, null);
      const timeline = rest.events;
      assert(timeline.every(event => event.operationId === trip.operationId));
      assert.deepEqual(timeline.map(event => event.type),
        floor === 7 ? ["complete", "wait"] : ["wait", "complete", "resume"]);
      const completion = timeline.find(event => event.type === "complete");
      assert.equal(completion.status, status);
      if (floor === 7) assert.equal(completion.reason, "LiftBusy");
      if (floor === 1) assert.equal(completion.reason, "TargetEndPlay");
      const wait = timeline.find(event => event.type === "wait");
      assert.equal(wait.suspended, floor !== 7);
      assert.equal(wait.waiter.instanceId, trip.caller.instanceId);
      assert(wait.waiter.line > 0 && trip.caller.line > 0);
    }
    console.log("read_trace verified against listen-server export " + values.trace);
  }
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
