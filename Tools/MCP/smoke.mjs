// Usage: node smoke.mjs --project <demo.uproject> --editor <UnrealEditor-Cmd.exe>
import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const { values } = parseArgs({ options: {
  project: { type: "string" }, editor: { type: "string" }, trace: { type: "string" },
  "timeout-trace": { type: "string" }, "checkpoint-trace": { type: "string" },
  "landing-write-trace": { type: "string" }, "landing-read-trace": { type: "string" },
  "cache-visits": { type: "string", multiple: true }
} });
const root = path.join(path.dirname(path.resolve(values.project)), "Content", "AngelIO");
const traceDir = path.join(path.dirname(path.resolve(values.project)), "Saved", "AngelIO", "Traces");
const client = new Client({ name: "angelio-smoke", version: "1" });
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [fileURLToPath(new URL("./server.mjs", import.meta.url)), "--project", values.project, "--editor", values.editor]
});
const call = (name, args = {}) => client.callTool({ name, arguments: args }, undefined, { timeout: 150000 });

let fixture;
try {
  await client.connect(transport);
  const descriptor = JSON.parse(await readFile(new URL("../../Plugins/AngelIO/AngelIO.uplugin", import.meta.url), "utf8"));
  const lock = JSON.parse(await readFile(new URL("./package-lock.json", import.meta.url), "utf8"));
  assert.equal(client.getServerVersion().version, descriptor.VersionName);
  assert.equal(lock.version, descriptor.VersionName);
  assert.equal(lock.packages[""].version, descriptor.VersionName);
  const tools = (await client.listTools()).tools.map(tool => tool.name);
  assert.deepEqual(tools.sort(), ["describe_api", "describe_scene", "list_scripts", "read_script", "read_trace", "validate_map", "validate_script", "write_script"]);
  assert((await call("read_trace", { trace: "../DefaultEngine.ini" })).isError);
  const newest = await call("read_trace", { limit: 1 });
  if (newest.isError) assert.match(newest.content[0].text, /No AngelIO trace found/);
  else assert(newest.structuredContent.events.length === 1 ||
    (newest.structuredContent.events.length === 0 && newest.structuredContent.bytecodeCache));
  for (const visits of values["cache-visits"] ?? []) {
    const files = visits.split(",");
    assert.equal(files.length, 2, "Pass the first-load and reload export basenames, separated by a comma");
    const snapshots = [];
    for (const trace of files) {
      const reply = await call("read_trace", { trace, limit: 1 });
      assert(!reply.isError, JSON.stringify(reply.content));
      const snapshot = reply.structuredContent;
      assert([4, 5].includes(snapshot.schemaVersion));
      assert.equal(snapshot.netMode, "listenServer");
      const cache = snapshot.bytecodeCache;
      const exported = JSON.parse(await readFile(path.join(traceDir, trace), "utf8"));
      assert.deepEqual(cache, exported.bytecodeCache);
      if (snapshot.schemaVersion === 5) {
        assert.equal(cache.residentScripts.reduce((sum, row) => sum + row.retainedBytes, 0), cache.retainedBytes);
        assert.equal(new Set(cache.residentScripts.map(row => row.script)).size, cache.residentScripts.length);
        assert.equal(cache.droppedActivity, 0);
        assert.equal(cache.recentActivity.length, cache.hits + cache.evictions);
        cache.recentActivity.forEach((row, i) => assert.equal(row.sequence, i + 1));
      }
      assert(cache.retainedBytes <= cache.budgetBytes);
      assert.equal(Boolean(cache.retainedBytes), Boolean(cache.budgetBytes));
      const empty = await call("read_trace", { trace, operationId: randomUUID(), afterSequence: Number.MAX_SAFE_INTEGER });
      assert(!empty.isError, JSON.stringify(empty.content));
      assert.deepEqual(empty.structuredContent.events, []);
      assert.deepEqual(empty.structuredContent.bytecodeCache, cache);
      snapshots.push(snapshot);
    }
    const [first, reload] = snapshots;
    assert.equal(first.sessionId, reload.sessionId);
    assert.equal(first.schemaVersion, reload.schemaVersion);
    assert.notEqual(first.trace, reload.trace);
    const before = first.bytecodeCache, after = reload.bytecodeCache;
    assert.equal(after.budgetBytes, before.budgetBytes);
    assert([0, 3072, 32768].includes(after.budgetBytes));
    assert.equal(after.hits - before.hits, after.budgetBytes === 32768 ? 4 : 0);
    assert.equal(after.evictions - before.evictions, after.budgetBytes === 3072 ? 4 : 0);
    if (reload.schemaVersion === 5) {
      const reloadActivity = after.recentActivity.filter(row => row.sequence > before.hits + before.evictions);
      const roomScripts = ["Elevator.as", "Elevator.as", "Examples/BlinkPanel.as", "Examples/RoomDelivery.as"];
      assert.deepEqual(reloadActivity.map(row => row.script).sort(), after.budgetBytes ? roomScripts : []);
      if (after.budgetBytes === 3072) {
        const victim = reloadActivity[0];
        assert.deepEqual(before.residentScripts, [{ script: victim.script, retainedBytes: victim.retainedBytes }]);
      }
      for (const row of reloadActivity) {
        assert.equal(row.type, after.budgetBytes === 32768 ? "hit" : "eviction");
        if (row.type === "hit") {
          assert.equal(row.retainedBytes, after.residentScripts.find(entry => entry.script === row.script).retainedBytes);
        }
      }
    }
    console.log("read_trace verified cache across reload at " + after.budgetBytes + " bytes: " + files.join(", "));
  }
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
    for (const schemaVersion of [1, 2, 3, 4, 5]) {
      const { sessionId, world, netMode, droppedEvents } = history;
      const sample = { schemaVersion, sessionId, world, netMode, droppedEvents, events: [calls[0]] };
      if (schemaVersion === 4) sample.bytecodeCache = { budgetBytes: 1024, retainedBytes: 900, hits: 1, evictions: 0 };
      if (schemaVersion === 5) sample.bytecodeCache = null;
      const trace = "00000000-000000-000-" + randomUUID().replaceAll("-", "") + ".json";
      const filename = path.join(traceDir, trace);
      await writeFile(filename, JSON.stringify(sample), { flag: "wx" });
      try {
        const page = await read({ trace, operationId: calls[0].operationId });
        assert.deepEqual(page, { trace, ...sample, nextAfterSequence: null });
      } finally {
        await rm(filename);
      }
    }
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
    assert([2, 3, 4, 5].includes(history.schemaVersion));
    assert.equal(history.netMode, "listenServer");
    const checkpointId = history.events.find(e => e.type === "checkpointSaved").checkpointId;
    const first = await read({ checkpointId, limit: 2 });
    assert.equal(first.nextAfterSequence, first.events[1].sequence);
    const next = await read({ checkpointId, afterSequence: first.nextAfterSequence });
    assert.equal(next.nextAfterSequence, null);
    const events = [...first.events, ...next.events];
    // Older exports predate the durable dispatch stage.
    const hasDispatch = events[0].stage === "Dispatched";
    const expected = hasDispatch ? [["checkpointSaved", "Dispatched", 10], ["checkpointRestored", "Dispatched", 6]] : [];
    expected.push(["checkpointSaved", "Delivered", hasDispatch ? 19 : 14], ["checkpointRestored", "Delivered", 6],
      ["checkpointSaved", "Complete", hasDispatch ? 26 : 21], ["checkpointRestored", "Complete", 6]);
    assert.deepEqual(events.map(e => [e.type, e.stage, e.caller.line]), expected);
    for (const event of events) {
      assert.equal(event.checkpointId, checkpointId);
      assert.equal(event.version, 1);
      assert.equal(event.caller.script, "Examples/RoomDelivery.as");
      assert.equal(event.caller.component, events[0].caller.component);
      assert(!("operationId" in event));
    }
    for (let i = 0; i < events.length; i += 2) {
      const [saved, restored] = events.slice(i, i + 2);
      assert.equal(saved.caller.instanceId, saved.savedByInstanceId);
      assert.equal(restored.savedByInstanceId, saved.caller.instanceId);
      assert.notEqual(restored.caller.instanceId, saved.caller.instanceId);
      if (i > 0) assert.equal(saved.caller.instanceId, events[i - 1].caller.instanceId);
    }
    const ride = history.events.find(e => e.operation === "Lift.MoveTo");
    const tripHistory = await read({ operationId: ride.operationId });
    assert(tripHistory.events.length && tripHistory.events.every(e => e.operationId === ride.operationId && !e.checkpointId));
    console.log("read_trace verified checkpoint saves and restores across room reloads in " + trace);
  }
  if (values["landing-write-trace"] || values["landing-read-trace"]) {
    const writer = values["landing-write-trace"], reader = values["landing-read-trace"];
    assert(writer && reader, "Pass both restart exports for a completed landing");
    const read = async (trace, args = {}) => {
      const response = await call("read_trace", { trace, ...args });
      assert(!response.isError, JSON.stringify(response.content));
      return response.structuredContent;
    };
    const before = await read(writer);
    const after = await read(reader);
    for (const history of [before, after]) {
      assert([3, 4, 5].includes(history.schemaVersion));
      assert.equal(history.netMode, "listenServer");
      assert.equal(history.droppedEvents, 0);
    }
    assert.notEqual(before.sessionId, after.sessionId);
    const freight = before.events.find(e => e.type === "stateSaved" && e.target === "FreightLift");
    assert(freight);
    const stateId = freight.stateId;
    const first = await read(writer, { stateId, limit: 1 });
    assert.equal(first.nextAfterSequence, first.events[0].sequence);
    const second = await read(writer, { stateId, afterSequence: first.nextAfterSequence });
    const restored = await read(reader, { stateId });
    const events = [...first.events, ...second.events, ...restored.events];
    assert.deepEqual(events.map(e => [e.type, e.value]), [["stateSaved", 2], ["stateSaved", 4], ["stateRestored", 4]]);
    assert(events.every(e => e.stateId === stateId && e.target === "FreightLift" && e.field === "floor" && e.component));
    assert.equal(restored.nextAfterSequence, null);
    assert(after.events.some(e => e.type === "stateRestored" && e.value === 3 && e.stateId !== stateId));
    assert(!after.events.some(e => e.operation === "Lift.MoveTo" || e.status === "Arrived"));
    const stages = before.events.filter(e => e.type === "checkpointSaved");
    if (stages.at(-1)?.stage === "Dispatched") {
      assert.equal(stages.length, 1);
      const dispatch = stages[0];
      const calls = before.events.filter(e => e.operation === "Lift.MoveTo");
      assert.equal(calls.length, 1);
      assert.equal(calls[0].caller.instanceId, dispatch.savedByInstanceId);
      assert.deepEqual(before.events.filter(e => e.status === "Arrived").map(e => e.operationId), [calls[0].operationId]);
      const recovery = (await read(reader, { checkpointId: dispatch.checkpointId })).events;
      assert.deepEqual(recovery.map(e => [e.type, e.stage]), [["checkpointRestored", "Dispatched"], ["checkpointSaved", "Delivered"]]);
      assert.equal(recovery[0].savedByInstanceId, dispatch.savedByInstanceId);
      assert.notEqual(recovery[1].savedByInstanceId, dispatch.savedByInstanceId);
      console.log("read_trace verified recovery between the landing and Delivered commits");
    }
    assert((await call("read_trace", { trace: reader, stateId, operationId: stateId })).isError);
    console.log("read_trace verified the saved and restored floor across listen-server processes: " + stateId);
  }
  assert((await call("list_scripts")).structuredContent.scripts.includes("Elevator.as"));
  const api = (await call("describe_api")).structuredContent;
  assert.equal(api.entry, "void Run()");
  assert(api.externalDeclarations.some(s => s.includes("Lift::MoveTo(int floor, float speed)")));
  assert(api.externalDeclarations.some(s => s.includes("Lift::HasSavedArrival(int floor)")));
  assert(api.externalDeclarations.some(s => s.includes("Door@ FindDoor(")));
  assert(api.externalDeclarations.some(s => s.includes("Door::Open()")));
  assert(api.externalDeclarations.some(s => s.includes("DoorOpen::Wait()")));
  assert(api.externalDeclarations.some(s => s.includes("LiftTrip::Wait()")));
  assert(api.externalDeclarations.some(s => s.includes("LiftTrip::Wait(float")));
  assert(api.externalDeclarations.includes("Lift@ WaitActor(const string &in name)"));
  assert(api.externalDeclarations.includes("Lift@ WaitActor(const string &in name, float timeoutSeconds)"));
  assert(!api.externalDeclarations.some(s => s.includes("AngelIOInternal::")));
  assert((await call("validate_script", { script: "Examples/StreamedLift.as" })).structuredContent.valid);
  const terminal = (await call("validate_script", { script: "Examples/DoorTerminal.as" })).structuredContent;
  assert(terminal.valid && terminal.inputSignals.includes("Activate") &&
    terminal.ports.some(p => p.name === "Opened" && p.type === "bool" && p.direction === "out"));
  const room = (await call("validate_map", { map: "/Game/Demo/DoorRoom" })).structuredContent;
  assert(room.valid && room.missionsChecked === 1, JSON.stringify(room));
  const doorScene = (await call("describe_scene", { map: "/Game/Demo/DoorRoom" })).structuredContent;
  assert(doorScene.components.some(c => c.script === "Examples/DoorTerminal.as" && c.roomTargets.Door === "RoomDoor"));
  assert(doorScene.components.some(c => c.targetName === "RoomDoor"));
  assert(api.calls.some(s => s.startsWith("string LoadCheckpoint(")));
  assert(api.calls.some(s => s.startsWith("void SaveCheckpoint(")));
  const delivery = (await call("validate_script", { script: "Examples/RoomDelivery.as" })).structuredContent;
  assert(delivery.valid && delivery.inputSignals.includes("Confirm") && delivery.ports.some(p => p.name === "Stage" && p.direction === "out"));
  const mapReport = (await call("validate_map", { map: "/Game/Demo/TwoLifts" })).structuredContent;
  assert(mapReport.valid && mapReport.missionsChecked === 4, JSON.stringify(mapReport));
  const scene = (await call("describe_scene", { map: "/Game/Demo/TwoLifts" })).structuredContent;
  assert(scene.actors.some(a => a.id && a.room && a.actorClass));
  assert(scene.components.some(c => c.targetName === "FreightLift" && scene.actors.some(a => a.id === c.actor)));
  assert(scene.components.some(c => c.script === "TypedLift.as" && c.roomTargets && c.initialInputs));
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
  const script = path.relative(root, invalid);
  const original = (await call("read_script", { script })).structuredContent;
  const edit = { script, source: "void Run() {}\n", expectedRevision: original.revision };
  const dry = (await call("write_script", edit)).structuredContent;
  assert(dry.valid && !dry.written && dry.revision === original.revision);
  const broken = (await call("write_script", { ...edit, source: original.source, dryRun: false })).structuredContent;
  assert.equal(broken.code, "compile_failed");
  assert.equal((await call("read_script", { script })).structuredContent.source, original.source);
  const saved = (await call("write_script", { ...edit, dryRun: false })).structuredContent;
  assert(saved.written && saved.revision === dry.proposedRevision);
  const stale = (await call("write_script", { ...edit, dryRun: false })).structuredContent;
  assert.equal(stale.code, "revision_conflict");
  assert.equal(stale.currentRevision, saved.revision);
  assert.equal((await call("read_script", { script })).structuredContent.source, edit.source);
  assert((await call("read_script", { script: "../Outside.as" })).isError);
  console.log("MCP stdio OK: discovery, source, API, mission and map validation, error locations, path scope.");
} finally {
  if (fixture) await rm(fixture, { recursive: true, force: true });
  await client.close();
}
