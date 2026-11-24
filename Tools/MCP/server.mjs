import { execFile } from "node:child_process";
import { mkdtemp, readFile, readdir, realpath, rm, stat } from "node:fs/promises";
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
const server = new McpServer({ name: "angelio", version: "0.1.0" });
const annotations = { readOnlyHint: true, openWorldHint: false };

function result(data) {
  return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }], structuredContent: data };
}

async function scriptPath(script) {
  if (path.isAbsolute(script) || /[:"\x00-\x1f]/.test(script) ||
      script.split(/[\\/]/).includes("..") || path.extname(script).toLowerCase() !== ".as")
    throw new Error("Expected a relative .as file under Content/AngelIO");
  const target = await realpath(path.resolve(root, script));
  const relative = path.relative(root, target);
  if (relative.startsWith(".." + path.sep) || path.isAbsolute(relative) || !(await stat(target)).isFile())
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
  return result({ script: relative, source: await readFile(target, "utf8") });
});

const traceFilename = /^\d{8}-\d{6}-\d{3}-[a-f0-9]{32}\.json$/i;
// FGuid uses 128 bits without RFC UUID version/variant constraints.
const guid = z.string().regex(/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/i);
const traceSchema = z.object({
  schemaVersion: z.literal(1), sessionId: guid, world: z.string(), netMode: z.string(),
  droppedEvents: z.number().int().nonnegative(), events: z.array(z.object({
    sequence: z.number().int().positive(), operationId: guid, worldSeconds: z.number(),
    type: z.enum(["call", "wait", "complete", "resume", "detach"])
  }).passthrough())
});

server.registerTool("read_trace", {
  description: "Read an exported operation history from Saved/AngelIO/Traces. Defaults to the latest export. Follow operationId from call to wait and completion. Reuse the returned trace filename when paging.",
  inputSchema: {
    trace: z.string().regex(traceFilename).optional().describe("Export filename. Omit to read the latest export."),
    operationId: guid.optional(),
    afterSequence: z.number().int().nonnegative().default(0),
    limit: z.number().int().min(1).max(200).default(50)
  }, annotations
}, async ({ trace, operationId, afterSequence, limit }) => {
  const directory = path.join(path.dirname(project), "Saved", "AngelIO", "Traces");
  const noExports = "No AngelIO trace found. Use AngelIO.ExportTrace on the host or end a PIE session with operation events.";
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
  const id = operationId?.toLowerCase();
  const matches = events.filter(event => event.sequence > afterSequence && (!id || event.operationId === id));
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
      "SendTo(const string &in target, const string &in input, int value) delivers to live components with this TargetName in the authoritative world.",
      "int SignalValue() reads the last delivered integer payload.",
      "Emit(OutputSignal name, int value) publishes after the next suspension or return."
    ],
    lifetime: "Each component owns its globals. An unmatched signal is dropped. StopScript and EndPlay cancel the instance.",
    addressing: "TargetName is an explicit case-insensitive logical name. Equal names form a group. Input signal names are case-sensitive. No match means no delivery.",
    network: "Standalone or server executes Run. Relevant clients observe replicated output values and waiting signal. OnSignal stays authoritative. Opted-in presentation signals are unreliable live effects; late joiners read current state."
  });
});

async function validate(selector = "-ApiOnly") {
  const dir = await mkdtemp(path.join(tmpdir(), "angelio-check-"));
  try {
    const report = path.join(dir, "report.json");
    try {
      await exec(editor, [project, "-run=AngelIOCheck", selector, "-Report=" + report,
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

server.registerTool("validate_map", {
  description: "Check missions on loaded actors and default player-controller components in a saved map. Reports script, initial input and literal named-target diagnostics without starting PIE.",
  inputSchema: { map: z.string().regex(/^\/Game\/(?:[\p{L}\p{N}_-]+\/)*[\p{L}\p{N}_-]+$/u)
    .describe("Saved map package, e.g. /Game/Demo/TwoLifts") }, annotations
}, async ({ map }) => result(await validate("-Map=" + map)));

await server.connect(new StdioServerTransport());
