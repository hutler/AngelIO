import { execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { createServer } from "node:net";
import { link, mkdtemp, readFile, readdir, realpath, rename, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { parseArgs, promisify } from "node:util";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const { values } = parseArgs({ options: {
  project: { type: "string" },
  editor: { type: "string" }
} });
if (!values.project || !values.editor) {
  throw new Error("Pass --project <file.uproject> --editor <UnrealEditor-Cmd.exe>");
}
const project = await realpath(values.project);
const editor = await realpath(values.editor);
const root = await realpath(path.join(path.dirname(project), "Content", "AngelIO"));
const exec = promisify(execFile);
const { version } = JSON.parse(await readFile(new URL("./package.json", import.meta.url), "utf8"));
const server = new McpServer({ name: "angelio", version });
const annotations = { readOnlyHint: true, openWorldHint: false };

function result(data) {
  return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }], structuredContent: data };
}

async function scriptPath(script, allowNew = false) {
  if (path.isAbsolute(script) || /[:"\x00-\x1f]/.test(script) ||
      script.split(/[\\/]/).includes("..") || path.extname(script).toLowerCase() !== ".as")
    throw new Error("Expected a relative .as file under Content/AngelIO");
  const parent = await realpath(path.resolve(root, path.dirname(script)));
  const entry = path.join(parent, path.basename(script));
  let target;
  try { target = await realpath(entry); }
  catch (error) { if (!allowNew || error.code !== "ENOENT") throw error; target = entry; }
  const relative = path.relative(root, target);
  if (relative === ".." || relative.startsWith(".." + path.sep) || path.isAbsolute(relative) || (!allowNew && !(await stat(target)).isFile()))
    throw new Error("Script resolves outside Content/AngelIO or is not a file");
  return { target, relative: relative.split(path.sep).join("/") };
}

async function listScripts(directory) {
  const scripts = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) scripts.push(...await listScripts(file));
    else if (entry.isFile() && path.extname(entry.name).toLowerCase() === ".as")
      scripts.push(path.relative(root, file).split(path.sep).join("/"));
  }
  return scripts.sort();
}

server.registerTool("list_scripts", {
  description: "List AngelIO mission scripts in this Unreal project.", annotations
}, async () => result({ scripts: await listScripts(root) }));

const inputSchema = { script: z.string().min(1).describe("Path relative to Content/AngelIO, e.g. Elevator.as") };
server.registerTool("read_script", {
  description: "Read the source of a project mission.", inputSchema, annotations
}, async ({ script }) => {
  const { target, relative } = await scriptPath(script);
  const bytes = await readFile(target);
  return result({ script: relative, source: bytes.toString("utf8"), revision: digest(bytes) });
});

const digest = bytes => createHash("sha256").update(bytes).digest("hex");
async function revision(file) {
  try { return digest(await readFile(file)); }
  catch (error) { if (error.code === "ENOENT") return null; throw error; }
}

server.registerTool("write_script", {
  description: "Compile a proposed mission using the game bindings. Dry runs leave the file untouched. Commit only if its SHA-256 still matches read_script. Supply null to create a new mission.",
  inputSchema: { ...inputSchema, source: z.string().min(1), expectedRevision: z.string().regex(/^[0-9a-f]{64}$/).nullable(), dryRun: z.boolean().default(true) },
  annotations: { readOnlyHint: false, openWorldHint: false, destructiveHint: true }
}, async ({ script, source, expectedRevision, dryRun }) => {
  const { target, relative } = await scriptPath(script, true);
  const conflict = current => result({ valid: false, written: false, code: "revision_conflict", script: relative, currentRevision: current });
  let current = await revision(target);
  if (current !== expectedRevision) return conflict(current);
  const report = await validate("-Script=" + relative, [], source);
  const nextRevision = digest(source);
  if (!report.valid) return result({ ...report, written: false, code: "compile_failed" });
  // Serialize the final compare-and-publish across local MCP processes. The OS
  // releases the listening socket when its process exits, including on a crash.
  const lock = createServer();
  const key = digest(process.platform === "win32" ? target.toLowerCase() : target);
  const socket = process.platform === "win32" ? "//./pipe/angelio-" + key : path.join(tmpdir(), "aio-" + key.slice(0, 32));
  try {
    await new Promise((resolve, reject) => { lock.once("error", reject); lock.listen(socket, resolve); });
  } catch (error) {
    if (error.code === "EADDRINUSE") return result({ valid: false, written: false, code: "write_busy", script: relative });
    throw error;
  }
  const temporary = path.join(path.dirname(target), ".angelio-" + randomUUID() + ".tmp");
  try {
    if (!dryRun) await writeFile(temporary, source, { flag: "wx" });
    current = await revision(target);
    if (current !== expectedRevision) return conflict(current);
    if (!dryRun) {
      if (expectedRevision === null) {
        try { await link(temporary, target); }
        catch (error) { if (error.code === "EEXIST") return conflict(await revision(target)); throw error; }
      } else await rename(temporary, target);
    }
    return result({ ...report, written: !dryRun, revision: dryRun ? current : nextRevision, proposedRevision: nextRevision });
  } finally {
    lock.close();
    if (!dryRun) await rm(temporary, { force: true });
  }
});

const traceFilename = /^\d{8}-\d{6}-\d{3}-[a-f0-9]{32}\.json$/i;
// FGuid uses 128 bits without RFC UUID version/variant constraints.
const guid = z.string().regex(/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/i);
const eventFields = { sequence: z.number().int().positive(), worldSeconds: z.number() };
const operationEvent = z.object({
  ...eventFields, operationId: guid, type: z.enum(["call", "wait", "complete", "resume", "detach"])
}).passthrough();
const checkpointEvent = z.object({
  ...eventFields, type: z.enum(["checkpointSaved", "checkpointRestored"]), checkpointId: guid,
  stage: z.string().min(1), version: z.number().int().positive(), savedByInstanceId: guid,
  caller: z.object({ instanceId: guid, script: z.string(), line: z.number().int(), component: z.string() }).passthrough()
}).passthrough();
const traceFields = { sessionId: guid, world: z.string(), netMode: z.string(), droppedEvents: z.number().int().nonnegative() };
const stateEvent = z.object({
  ...eventFields, type: z.enum(["stateSaved", "stateRestored"]), stateId: guid,
  target: z.string(), component: z.string(), field: z.string(), value: z.number().int()
});
const traceSchema = z.discriminatedUnion("schemaVersion", [
  z.object({ ...traceFields, schemaVersion: z.literal(1), events: z.array(operationEvent) }),
  z.object({ ...traceFields, schemaVersion: z.literal(2), events: z.array(z.union([operationEvent, checkpointEvent])) }),
  z.object({ ...traceFields, schemaVersion: z.literal(3), events: z.array(z.union([operationEvent, checkpointEvent, stateEvent])) })
]);

server.registerTool("read_trace", {
  description: "Read exported mission events from Saved/AngelIO/Traces. Filter rides by operationId, mission stages by checkpointId, or saved and restored lift floors by stateId. Each server session has its own export. Reuse the trace filename when paging.",
  inputSchema: {
    trace: z.string().regex(traceFilename).optional().describe("Export filename. Omit to read the latest export."),
    operationId: guid.optional(),
    checkpointId: guid.optional().describe("Checkpoint ID from the mission inspector or a checkpoint event."),
    stateId: guid.optional().describe("State ID from the inspector or a stateSaved/stateRestored event. Use the same ID in both server exports."),
    afterSequence: z.number().int().nonnegative().default(0),
    limit: z.number().int().min(1).max(200).default(50)
  }, annotations
}, async ({ trace, operationId, checkpointId, stateId, afterSequence, limit }) => {
  if ([operationId, checkpointId, stateId].filter(Boolean).length > 1) throw new Error("Choose one of operationId, checkpointId or stateId.");
  const directory = path.join(path.dirname(project), "Saved", "AngelIO", "Traces");
  const noExports = "No AngelIO trace found. Use AngelIO.ExportTrace on the host or end a PIE session with mission events.";
  let entries;
  try { entries = await readdir(directory, { withFileTypes: true }); }
  catch (error) { if (error.code === "ENOENT") throw new Error(noExports); throw error; }
  const names = entries.filter(entry => entry.isFile() && traceFilename.test(entry.name)).map(entry => entry.name);
  const filename = trace ?? names.sort().at(-1);
  if (!filename) throw new Error(noExports);
  if (!names.includes(filename)) throw new Error("Trace export not found: " + filename);
  const target = await realpath(path.join(directory, filename));
  if (path.relative(directory, target) !== filename) throw new Error("Trace resolves outside the project trace directory");
  const { events, ...metadata } = traceSchema.parse(JSON.parse(await readFile(target, "utf8")));
  const operation = operationId?.toLowerCase();
  const checkpoint = checkpointId?.toLowerCase();
  const state = stateId?.toLowerCase();
  const matches = events.filter(event => event.sequence > afterSequence &&
    (!operation || event.operationId === operation) && (!checkpoint || event.checkpointId === checkpoint) &&
    (!state || event.stateId === state));
  const page = matches.slice(0, limit);
  return result({ trace: filename, ...metadata, events: page,
    nextAfterSequence: matches.length > limit ? page.at(-1).sequence : null });
});

server.registerTool("describe_api", {
  description: "Show the AngelIO entry point, variable conventions, VM calls and signal delivery rules.", annotations
}, async () => {
  const report = await validate();
  if (!report.valid) throw new Error("AngelIO API registration failed: " + JSON.stringify(report.diagnostics));
  return result({
    externalDeclarations: report.externalDeclarations,
    entry: "void Run()", portTypes: ["int", "float", "bool"],
    inputs: "in_Name globals can be set by the component. out_Name globals are readable outputs.",
    signals: "Declare InputSignal and OutputSignal enums at global scope. Use 32-bit enums with unique values.",
    calls: [
      "WaitFor(InputSignal value) suspends until this component receives that signal.",
      "WaitSeconds(float seconds) waits in game time. Fork(MissionBranch@ entry) runs a child until its first wait. EndOn(InputSignal s) cancels that child when its component receives s. Notify(InputSignal s, int value = 0) queues a signal on the same component.",
      "SendTo(const string &in target, const string &in input, int value) delivers to live components with this TargetName in the authoritative world.",
      "int SignalValue() reads the last delivered integer payload.",
      "string LoadCheckpoint(int version = 1) reads this room component's saved stage. An absent record or a version mismatch returns an empty string.",
      "void SaveCheckpoint(const string &in stage, int version = 1) commits a non-empty stage to the host's checkpoint slot. Use a positive version. A storage error fails the mission.",
      "Emit(OutputSignal name, int value) publishes after the next suspension or return."
    ],
    lifetime: "Each component owns its globals. An unmatched signal is dropped. StopScript and EndPlay cancel the instance. Explicit checkpoints survive room unloading and server restarts on the same map and named save slot. The default slot is Default.",
    addressing: "TargetName is an explicit case-insensitive logical name. Equal names form a group. Input signal names are case-sensitive. No match means no delivery.",
    network: "Standalone or server executes Run. Relevant clients observe replicated output values and waiting signal. OnSignal stays authoritative. Opted-in presentation signals are unreliable live effects; late joiners read current state."
  });
});

async function validate(selector = "-ApiOnly", args = [], candidate) {
  const dir = await mkdtemp(path.join(tmpdir(), "angelio-check-"));
  try {
    const report = path.join(dir, "report.json");
    if (candidate !== undefined) {
      const file = path.join(dir, "candidate.as");
      await writeFile(file, candidate);
      args = [...args, "-Candidate=" + file];
    }
    try {
      await exec(editor, [project, "-run=AngelIOCheck", selector, ...args, "-Report=" + report,
        "-unattended", "-nullrhi", "-nosound", "-nop4", "-nosplash", "-ddc=InstalledNoZenLocalFallback",
        "-abslog=" + path.join(dir, "check.log")],
        { timeout: 120000, maxBuffer: 8 * 1024 * 1024, windowsHide: true });
    } catch (error) {
      // Validation errors return 1 with structured diagnostics.
      if (error.code !== 1) throw new Error("Unreal validator failed: " + (error.code ?? error.message));
    }
    try { return JSON.parse(await readFile(report, "utf8")); }
    catch { throw new Error("Unreal did not produce a compiler report. Build the project's Editor target with AngelIO enabled."); }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

server.registerTool("validate_script", {
  description: "Compile a mission with the project's Unreal plugin. Returns valid, typed ports, signal names and line/column diagnostics. Does not execute the mission.",
  inputSchema, annotations
}, async ({ script }) => {
  const { relative } = await scriptPath(script);
  return result(await validate("-Script=" + relative));
});

const mapSchema = { map: z.string().regex(/^\/Game\/(?:[\p{L}\p{N}_-]+\/)*[\p{L}\p{N}_-]+$/u)
  .describe("Saved map package, e.g. /Game/Demo/TwoLifts") };
server.registerTool("validate_map", {
  description: "Check missions on loaded actors and default player-controller components in a saved map. Reports script, initial input and literal named-target diagnostics without starting PIE.",
  inputSchema: mapSchema, annotations
}, async ({ map }) => result(await validate("-Map=" + map)));

server.registerTool("describe_scene", {
  description: "Describe saved map actors, placed mission components, controller templates, inputs and room target bindings by Unreal object path, with validation diagnostics.",
  inputSchema: mapSchema, annotations
}, async ({ map }) => result(await validate("-Map=" + map, ["-DescribeScene"])));

await server.connect(new StdioServerTransport());
