// Usage: node smoke.mjs --project <demo.uproject> --editor <UnrealEditor-Cmd.exe>
import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const { values } = parseArgs({ options: {
  project: { type: "string" }, editor: { type: "string" }, trace: { type: "string" },
  "timeout-trace": { type: "string" }, "checkpoint-trace": { type: "string" }
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
  assert.deepEqual(tools.sort(), ["describe_api", "list_scripts", "read_script", "read_trace", "validate_map", "validate_script"]);
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
  if (values["timeout-trace"]) {
    const trace = values["timeout-trace"];
    const history = (await call("read_trace", { trace })).structuredContent;
    assert.equal(history.netMode, "listenServer");
    const ride = history.events.find(e => e.type === "call" && e.caller.script === "CachedLift.as" && e.arguments.floor === 4);
    const timeline = (await call("read_trace", { trace, operationId: ride.operationId })).structuredContent.events;
    const poll = timeline.find(e => e.type === "wait" && e.outcome === "TimedOut");
    assert(poll.timeoutSeconds === 0 && !poll.suspended);
    const timed = timeline.find(e => e.type === "resume" && e.outcome === "TimedOut");
    assert(timeline.some(e => e.type === "wait" && e.timeoutSeconds > 0 && e.waiter.line === timed.waiter.line));
    const completions = timeline.filter(e => e.type === "complete");
    assert.equal(completions.length, 1);
    assert(completions[0].sequence > timed.sequence && completions[0].status === "Arrived");
    assert(history.events.some(e => e.status === "Cancelled" && e.reason === "TargetGone"));
    console.log("read_trace verified waiter timeout and eventual arrival in " + trace);
  }
  if (values["checkpoint-trace"]) {
    const trace = values["checkpoint-trace"];
    const read = async (args = {}) => {
      const reply = await call("read_trace", { trace, ...args });
      assert(!reply.isError, JSON.stringify(reply.content));
      return reply.structuredContent;
    };
    const history = await read({ limit: 200 });
    assert.equal(history.schemaVersion, 2);
    assert.equal(history.netMode, "listenServer");
    const checkpointId = history.events.find(e => e.type === "checkpointSaved").checkpointId;
    const first = await read({ checkpointId, limit: 2 });
    assert.equal(first.nextAfterSequence, first.events[1].sequence);
    const next = await read({ checkpointId, afterSequence: first.nextAfterSequence });
    assert.equal(next.nextAfterSequence, null);
    const events = [...first.events, ...next.events];
    assert.deepEqual(events.map(e => [e.type, e.stage, e.caller.line]), [
      ["checkpointSaved", "Delivered", 14], ["checkpointRestored", "Delivered", 6],
      ["checkpointSaved", "Complete", 21], ["checkpointRestored", "Complete", 6]
    ]);
    for (const event of events) {
      assert.equal(event.checkpointId, checkpointId);
      assert.equal(event.version, 1);
      assert.equal(event.caller.script, "Examples/RoomDelivery.as");
      assert.equal(event.caller.component, events[0].caller.component);
      assert(!("operationId" in event));
    }
    for (const [saved, restored] of [[events[0], events[1]], [events[2], events[3]]]) {
      assert.equal(saved.caller.instanceId, saved.savedByInstanceId);
      assert.equal(restored.savedByInstanceId, saved.caller.instanceId);
      assert.notEqual(restored.caller.instanceId, saved.caller.instanceId);
    }
    assert.equal(events[1].caller.instanceId, events[2].caller.instanceId);
    const ride = history.events.find(e => e.operation === "Lift.MoveTo");
    const tripHistory = await read({ operationId: ride.operationId });
    assert(tripHistory.events.length && tripHistory.events.every(e => e.operationId === ride.operationId && !e.checkpointId));
    console.log("read_trace verified checkpoint saves and restores across room reloads in " + trace);
  }
  assert((await call("list_scripts")).structuredContent.scripts.includes("Elevator.as"));
  const api = (await call("describe_api")).structuredContent;
  assert.equal(api.entry, "void Run()");
  assert(api.externalDeclarations.some(s => s.includes("Lift::MoveTo(int floor, float speed)")));
  assert(api.externalDeclarations.some(s => s.includes("LiftTrip::Wait()")));
  assert(api.externalDeclarations.some(s => s.includes("LiftTrip::Wait(float")));
  assert(api.externalDeclarations.includes("Lift@ WaitActor(const string &in name)"));
  assert(api.externalDeclarations.includes("Lift@ WaitActor(const string &in name, float timeoutSeconds)"));
  assert(!api.externalDeclarations.some(s => s.includes("AngelIOInternal::")));
  assert((await call("validate_script", { script: "Examples/StreamedLift.as" })).structuredContent.valid);
  assert(api.calls.some(s => s.startsWith("string LoadCheckpoint(")));
  assert(api.calls.some(s => s.startsWith("void SaveCheckpoint(")));
  const delivery = (await call("validate_script", { script: "Examples/RoomDelivery.as" })).structuredContent;
  assert(delivery.valid && delivery.inputSignals.includes("Confirm") && delivery.ports.some(p => p.name === "Stage" && p.direction === "out"));
  const mapReport = (await call("validate_map", { map: "/Game/Demo/TwoLifts" })).structuredContent;
  assert(mapReport.valid && mapReport.missionsChecked === 4, JSON.stringify(mapReport));
  const missing = (await call("validate_map", { map: "/Game/Demo/NoSuchAngelIOMap" })).structuredContent;
  assert(!missing.valid && missing.diagnostics.some(d => d.code === "map_load"));
  assert((await call("validate_map", { map: "/Game/../Engine" })).isError);
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
  console.log("MCP stdio OK: discovery, source, API, mission and map validation, error locations, path scope.");
} finally {
  if (fixture) await rm(fixture, { recursive: true, force: true });
  await client.close();
}
