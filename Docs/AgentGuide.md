# AngelIO agent guide

Use this guide to change a level mission, expose a game feature to AngelScript, or diagnose a trip that appears to have stalled. The examples use the `/Game/Demo/TwoLifts` map and the bindings shipped with `AngelIOHost`. Another game can load the same plugin with a different set of bindings. Query its `describe_api` output before writing the mission.

| Task | Start with | Evidence to collect |
| --- | --- | --- |
| Change what a panel does | [Mission authoring](#mission-authoring) | Compiler report, then the changed output or signal in PIE |
| Add a native action that a script can await | [Typed operations](#typed-operations) | Registered declaration, distinct operation IDs, and results for overlapping requests |
| Explain a specific ride | [Listen-server investigation](#listen-server-investigation) | Server wait site and a trace filtered to the ride's ID |
| Work through MCP | [MCP tool reference](#mcp-tool-reference) | The tool's structured result or error |
| Reproduce a change from a checkout | [Build and validation](#build-and-validation) | Editor build, relevant test names and outcomes |

## Mission authoring

A `UAngelIOScriptComponent` owns one mission instance. The `Script` property is relative to the project's `Content/AngelIO` directory. For example, `Script = "TypedLift.as"` loads `Content/AngelIO/TypedLift.as`. Two components using the same file have separate globals, waits, and instance IDs. Start with the existing mission nearest to your task. [Typed caller](../Content/AngelIO/TypedLift.as) · [Lift executor](../Content/AngelIO/Elevator.as) · [Group panel](../Content/AngelIO/LiftPanel.as)

| Script declaration or call | Runtime contract |
| --- | --- |
| `void Run()` | Entry point. Runs on the game thread and can suspend. A loop that exceeds the execution budget before yielding fails with a diagnostic. |
| `in_Speed`, `out_LastFloor` | Public globals in the default namespace. Supported types are `int`, `float`, and `bool`. Inputs must be mutable. C++ and Blueprint use the case-sensitive names `Speed` and `LastFloor`. |
| `InputSignal`, `OutputSignal` | Optional global 32-bit enums. Each member needs a distinct value within its enum. Signal names are case-sensitive and carry one `int` payload. |
| `WaitFor(InputSignal::CallLift)` | Suspends until this instance receives that input. `SignalValue()` reads the integer delivered with it. Sending a signal before its matching wait does not queue it for later. |
| `Emit(OutputSignal::Arrived, floor)` | Queues an outgoing event. The component dispatches events in order after the script suspends or returns. A subscriber can respond to the next wait in the same frame. |
| `SendTo("DemoLifts", "RequestFloor", floor)` | Queues a named input for the matching components in the authoritative world. Targets can also subscribe to `OnNamedInput` in C++ or Blueprint. |

**Worked example.** This single-use panel waits for a floor, calls the named lift, and publishes a result. A missing or ambiguous lift leaves `ArrivedFloor` at zero and completes the script. The demo's `TypedLift.as` shows how to accept repeated requests and expose rejection or cancellation as an output.

```angelscript
enum InputSignal { RequestRide }
float in_Speed = 400.0f;
int out_ArrivedFloor = 0;

void Run()
{
    WaitFor(InputSignal::RequestRide);
    int floor = SignalValue();
    Lift@ lift = FindLift("FreightLift");
    if (lift is null) return;

    LiftTrip@ trip = lift.MoveTo(floor, in_Speed);
    TripResult@ result = trip.Wait();
    if (result.Status == TripStatus::Arrived)
        out_ArrivedFloor = result.Floor;
}
```

To exercise the example, save it as `Content/AngelIO/Examples/RequestRide.as`, attach a script component to an actor on the demo map, and set its `Script` to `Examples/RequestRide.as`. On the server, wait until the demo lifts finish their startup rides and the new component reports `RequestRide`. Call `SendSignal("RequestRide", 6)` on that component. After arrival, `GetOutput("ArrivedFloor", Value)` should succeed and `Value.Integer` should equal `6`. Enable replication on the owning actor if a client needs the output.

Set `InitialInputs` before `StartScript` to override defaults. At runtime, call `SetInput` on the authoritative component, check the return value, then deliver the signal that consumes it. An input type mismatch or an unknown port fails. For the first outgoing event, bind the delegate before starting the script. `StartScript` recompiles the file and creates a new instance. Restart PIE or explicitly restart the component after an edit. Keep the previous operation ID if you need to investigate a ride across a caller restart. [Component implementation](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOScriptComponent.cpp) · [Compiler and VM](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOProgram.cpp)

### Resolve names in the correct world

Set the logical `TargetName` on the component. Renaming the actor in the Outliner changes its label. To change its address during play, call `SetTargetName`. Registration follows `BeginPlay` and `EndPlay` on authority. Target names use Unreal's case-insensitive `FName` semantics. The map uses both forms of addressing below. [World registry](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOTargetRegistry.cpp) · [Demo lookup](../Source/AngelIOHost/AngelIOHost.cpp)

- **One actor.** The `Address` components identify `FreightLift` and `ServiceLiftB`. `FindLift` returns a handle when exactly one distinct lift actor matches in the calling mission's world. Check for `null` before `MoveTo`. A cached handle still addresses that actor after a rename. Resolve again to address an actor that takes over its name.
- **A group.** Both lifts' `Mission` components have `TargetName = DemoLifts`. `SendTo` delivers to the matching components. Use this for a broadcast such as a floor request. Use a typed trip handle when the caller needs a particular ride's result.

## Typed operations

The reusable contract lives in `AngelIO`. The host module defines `Lift`, `LiftTrip`, `TripResult`, `TripStatus`, the floor range, and the movement rules. A project that copies only `Plugins/AngelIO` supplies its own game provider. [Public extension interface](../Plugins/AngelIO/Source/AngelIO/Public/AngelIOScriptAPI.h) · [Full binding implementation](../Source/AngelIOHost/AngelIOHost.cpp)

1. Add `AngelIO` to the game's module dependencies. Implement `IAngelIOScriptAPI::RegisterBindings(asIScriptEngine&)`. Register the provider with `IModularFeatures` in `StartupModule` and unregister it in `ShutdownModule`. Keep it loaded while its VMs exist. Each compile calls registration on a fresh engine, including the validation commandlet.
2. Register the script types, reference-count behaviors, enum values, functions, and methods. Return failure if a registration fails. The lift provider uses a weak actor reference for the target and a shared reference for the trip. A script can keep a result after the actor disappears. Obtain the executing world with `AngelIO::GetScriptWorld()` inside a binding.
3. Create a new `FAngelIOOperation` for every invocation, including a rejected request. Pass the world, operation name, logical target, JSON arguments, and optionally the executing component or actor as `TraceTarget`. In the demo the target string is `FreightLift` and the object is its `Mission` component. That object lets the inspector associate a request with the exact executor even when names are shared or changed.
4. Have the producer retain the pending operation. Save the domain result before calling `MarkComplete(Status, Reason)` on the game thread. Cover early rejection, synchronous completion, normal arrival and target teardown. A stale lift handle yields `Cancelled` with `TargetGone`. Destroying a moving lift yields `Cancelled` with `TargetEndPlay`. [Trip result](../Source/AngelIOHost/AngelIOLiftTrip.cpp) · [Producer lifecycle](../Source/AngelIOHost/AngelIOExampleLift.cpp)
5. Implement the script's wait method with `AngelIO::AwaitOperation` on the same operation object. The timeout overload accepts an optional duration in game seconds and returns a shared outcome for this wait. Keep it in the script result handle. A completed operation returns inline, zero polls, and a positive deadline resumes on the first eligible game tick. Completion wins if both are observed on that tick. A timed-out `TripResult.Status` stays `TimedOut` after another reader gets `Arrived`. Stopping a waiter detaches that script. The producer and other waiters keep their references. [Independent waiters and trace retention tests](../Source/AngelIOHost/Tests/AngelIOOperationTraceTests.cpp)

Rebuild the editor after changing a binding. Check the new signature in `describe_api.externalDeclarations`, compile a small caller, then exercise the behavior. The demo result exposes `Id`, `Floor` and `Status`. Reasons such as `LiftBusy` appear in the trace's `complete` event. Use `trip.Id` or `result.Id` to correlate the outcome with the native operation's `GetOperationId()`. Two invocations on the same actor and floor still have distinct IDs.

### Streamed targets

Use `Lift@ lift = WaitActor("FreightLift", 5.0f)` in a persistent mission. Handle `null` on timeout. The lookup uses the authoritative world's registry and waits for one distinct lift to finish startup motion and accept floor requests. Missing or ambiguous names can become ready on a later tick. When both readiness and expiry become visible on the same update, readiness wins. Omitting the timeout waits indefinitely, zero polls, and a negative or non-finite duration raises a script error. Use the replicated panel wait and the inspector to distinguish `WaitActor` from `LiftTrip.Wait`. In `read_trace`, the lookup ends in `Ready`, `TimedOut` or `Cancelled / WaiterDetached`. A resulting `Lift.MoveTo` has its own ID. [Example mission](../Content/AngelIO/Examples/StreamedLift.as)

The game module defines lift readiness. Other providers can use `FindUniqueActor` on the world registry, supply a side-effect-free predicate to `AngelIO::AwaitCondition`, and return a typed result after resumption with `GetScriptWrappers`. The waiting VM checks the predicate on the game thread. `describe_api` includes the public wrappers. A streamed target can be absent from the saved map when a mission using `WaitActor` is validated.

Each typed lift handle captures the actor's play generation. The lift increments it in `EndPlay`. When a streamed level becomes hidden, Unreal can call `BeginPlay` again on the same actor. Treat `TargetUnloaded` as the terminal reason for a ride interrupted by level removal and `TargetGone` as the reason for calling through the expired handle. A persistent caller can call `WaitActor` again. If its own component belongs to the unloaded room, its VM detaches on removal. [Listen-server streaming regressions](../Source/AngelIOHost/Tests/AngelIOListenServerTests.cpp)

### Restore a streamed mission's checkpoint

`SaveCheckpoint(stage, version)` synchronously replaces the checkpoint for the executing component and script in the authoritative world. The stage is a nonempty name and the version is a positive integer, defaulting to 1. `LoadCheckpoint(version)` returns the saved name for that version, or `""` for a fresh or incompatible record. Call these from a component-backed `Run()` or its callees. Saving at a later version replaces the older record.

Place the save after the action that establishes the stage. For a native operation, check its terminal result first. On re-entry, branch in `Run()` to re-establish waits and publish the outputs appropriate for that stage. Locals, handles and pending operations belong to the old VM. A checkpoint stores the stage name. An interrupted action follows the retry or recovery logic of the last committed stage. Queued `Emit` and `SendTo` events dispatch when the VM yields or returns, so use a completion signal when delivery itself needs confirmation.

Keys use the level-instance package, the actor/component object path inside that level and the script path. Keep these identities stable when recreating a room. Two copies of the map keep separate records. `TargetName` can change independently. Checkpoints live in server memory until world teardown. A late client observes the restored mission's replicated outputs and waiting signal. [Working mission](../Content/AngelIO/Examples/RoomDelivery.as) · [Listen-server scenario](../Source/AngelIOHost/Tests/AngelIOListenServerTests.cpp)

## Listen-server investigation

Run **Play As Listen Server** with **2 players** and **Run Under One Process** on `/Game/Demo/TwoLifts`. The host runs both players' missions. Each player owns a separate `TripPanel` on their controller. Client input travels through the owning controller's server RPC. `GetOutput` and `GetWaitingSignal` on the client read the latest replicated public state. Use `OnPublicStateChanged` for persistent UI. Opted-in `OnPresentationSignal` effects use unreliable multicast and can arrive before state updates. Late joiners receive current state. [Controller and RPC checks](../Source/AngelIOHost/AngelIOPlayerController.cpp) · [Replication and callbacks](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOScriptComponent.cpp)

1. Wait for the two startup rides to finish. In the connected client's console enter `LiftTrip 6 100`. Open **Tools → AngelIO Mission Inspector**, select **Listen server**, then the remote player's **TripPanel**. Read the current source line, wait label (`LiftTrip.Wait`), and full operation ID.
2. Select the `FreightLift` actor's **Mission**. The lift script waits for `MotionFinished` while the caller waits on the ride handle. Both selections show the same operation ID. With the supplied scripts the caller invokes `MoveTo` at `TypedLift.as:21`, waits at `TypedLift.as:22`, and the executor waits at `Elevator.as:25`. Use the displayed location if the scripts have changed.
3. During the ride, run `LiftTrip 7 800` from the **host** console. Its idle panel can issue a second request. The lift rejects it with `LiftBusy`. Select that operation to read its result, then pin the original ID to follow the first ride through `Arrived` and the caller's resumption. A second command from the already-waiting panel is discarded by the controller before it creates a trip.
4. For an offline record, run `AngelIO.ExportTrace` in the host console or stop PIE. Copy the filename printed by `LogAngelIOTrace` from `Saved/AngelIO/Traces`. Correlate the trip through MCP as described below.

**Client view.** The inspector can resolve a replica to the server VM through the in-process IP NetDriver's NetGUID cache. It labels the local replicated wait separately. When the host is outside the process or the network backend cannot resolve the replica, only the replicated wait is available on the client. Inspect the authoritative PIE world for the script stack and full timeline. [Inspector](../Plugins/AngelIO/Source/AngelIOEditor/Private/SAngelIOMissionInspector.cpp) · [Executable test scenario](../Source/AngelIOHost/Tests/AngelIOListenServerTests.cpp)

## MCP tool reference

From the checkout root run `npm ci --prefix Tools/MCP`. Configure a stdio server whose command is `node` and whose arguments are the absolute path to `Tools/MCP/server.mjs`, `--project`, the absolute `.uproject` path, `--editor`, and the absolute `UnrealEditor-Cmd.exe` path. `describe_api`, `validate_script` and `validate_map` launch the built editor commandlet, so allow time for startup. The reference client allows 150 seconds per call. [Client configuration example](../README.md#mcp-tools-for-editors-and-agents) · [Server schemas](../Tools/MCP/server.mjs)

| Tool | Arguments | Read from `structuredContent` |
| --- | --- | --- |
| `list_scripts` | `{}` | `scripts`, sorted paths relative to `Content/AngelIO` |
| `read_script` | `{"script":"TypedLift.as"}` | Normalized `script` and UTF-8 `source` |
| `describe_api` | `{}` | `entry`, `portTypes`, `calls`, `externalDeclarations` and delivery rules for this project |
| `validate_script` | `{"script":"TypedLift.as"}` | `valid`, `ports` with name/type/direction, `inputSignals`, `outputSignals`, `externalDeclarations`, `diagnostics` |
| `validate_map` | `{"map":"/Game/Demo/TwoLifts"}` | `map`, `valid`, `missionsChecked`, and diagnostics with `code`, `severity`, `message`, `actor`, `component`, `script`, `field`, `line`, `column` |
| `read_trace` | Optional fields are `trace` (export basename), `operationId` (hyphenated GUID), `afterSequence` (exclusive, default 0), `limit` (1–200, default 50) | `trace`, `schemaVersion`, `sessionId`, `world`, `netMode`, `droppedEvents`, `events`, `nextAfterSequence` |

**Editing loop.** Call `list_scripts`, `describe_api`, and `read_script`. Edit the file with the workspace editor, then send its relative path to `validate_script`. A compiler error is a successful tool response with `valid: false`. Read each diagnostic's `severity`, `message`, `line` and `column`. A path or tool execution error sets `isError`. A valid report confirms compilation and public declarations. Use PIE for actor lookup, movement, replication and completion.

**Map checks.** Use **Tools → Validate AngelIO Missions** for the current editor world, or `validate_map` for a saved package. Each distinct script compiles once per check. `input_unknown` and `input_type` identify the component's initial override. `target_missing` and `target_ambiguous` identify a direct literal lookup and its source line. Providers can register global lookup names, actor types and the required cardinality through `GetMapLookups`. Use a unique global function name for each addressing rule. The static check recognizes unqualified calls with a plain double-quoted first argument. Escaped strings, expressions and names shadowed by a script function or method are skipped. The validator examines loaded placed actors and the configured player controller's mission templates. Exercise computed names and streamed or spawned targets in PIE.

**Trace lookup.** Call `read_trace` with `{}` for the latest snapshot, or pass the basename from the host log. Preserve the returned `trace` for every subsequent call. Find the `call` with the desired `operation`, `target`, `arguments` and `caller`. Pass its `operationId` and the same `trace` to read the ride. If `nextAfterSequence` is a number, pass it as `afterSequence` with the same filters and repeat until it is `null`. The filename pins the snapshot even when another PIE world exports during paging.

| Observed event sequence | Interpretation |
| --- | --- |
| `call → wait → complete → resume` | The waiter suspended. It resumed after the producer stored the result. |
| `call → complete → wait` with `suspended: false` | The result was ready when read. Inspect `status` and `reason` for arrival or rejection. |
| `wait → resume` with `outcome: TimedOut` | The reader's game-time deadline expired. Look for the producer's `complete` separately. A zero-time poll puts `outcome` directly on the `wait` event. |
| `wait → detach` | This waiting VM was released. Other waiters and the operation can still finish. |

Check `netMode` and `sessionId` before comparing histories. The world keeps the latest 4,096 events. A positive `droppedEvents` means the captured prefix is missing. A snapshot taken during motion can end at `wait`. Export again after the observed completion to inspect later events. MCP reads snapshots from disk. The live PIE view is in the editor tab. [Trace storage and export](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOTraceSubsystem.cpp)

## Build and validation

Run the PowerShell blocks from the directory containing `AngelIO.uproject`. The demo targets Unreal Engine 5.8.4 on Windows x64. Use an Unreal-compatible Visual Studio C++ toolchain. The Win64 AngelScript library is checked in. Node.js and npm are needed for the MCP steps. Set `UE_ROOT` to the directory containing `Engine` on your machine. Keep the following variables in the same PowerShell session. [Plugin build rules](../Plugins/AngelIO/Source/AngelIO/AngelIO.Build.cs) · [Optional SDK rebuild](../Tools/BuildAngelScript.ps1)

```powershell
$ue = $env:UE_ROOT
if (-not $ue) { throw 'Set UE_ROOT to the Unreal Engine installation containing Engine/.' }
$project = (Resolve-Path .\AngelIO.uproject).Path
$build = Join-Path $ue 'Engine/Build/BatchFiles/Build.bat'
$editor = Join-Path $ue 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'

& $build AngelIOHostEditor Win64 Development "-Project=$project" -WaitMutex
if ($LASTEXITCODE -ne 0) { throw 'Editor build failed' }
```

For changes to a runtime module or public header, also build the game target to catch editor-only dependencies.

```powershell
& $build AngelIOHost Win64 Development "-Project=$project" -WaitMutex
if ($LASTEXITCODE -ne 0) { throw 'Game build failed' }
```

**Validate a file directly.** Substitute the relative mission path for `TypedLift.as`. The commandlet compiles without executing global initializers or `Run`. Its exit codes are 0 for valid, 1 for invalid, and 2 for bad arguments or a report write failure. A contract-wide error can report line and column zero. Use `-ApiOnly` in place of `-Script=...` to inspect registration. [Commandlet](../Plugins/AngelIO/Source/AngelIOEditor/Private/AngelIOCheckCommandlet.cpp)

```powershell
$report = Join-Path $env:TEMP ("AngelIO-Check-" + [guid]::NewGuid() + ".json")
& $editor $project -run=AngelIOCheck '-Script=TypedLift.as' "-Report=$report" `
    -unattended -nullrhi -nosound -nop4 -ddc=InstalledNoZenLocalFallback
$validatorExit = $LASTEXITCODE
if (!(Test-Path -LiteralPath $report)) { throw "Missing compiler report (exit $validatorExit)" }
$contract = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
$contract.diagnostics
if ($validatorExit -ne 0 -or !$contract.valid) { throw "Validation failed. See $report" }
$contract.ports
```

**Run Unreal Automation.** The full filter below covers the tests in the table below. For a focused run change `$filter` to one of the narrower filters in the table. Confirm that the intended test names appear with `Result={Success}` in the log.

```powershell
$filter = 'AngelIO.'
$log = Join-Path $env:TEMP ("AngelIO-Automation-" + [guid]::NewGuid() + ".log")
& $editor $project /Game/Demo/TwoLifts "-ExecCmds=Automation RunTests $filter;Quit" `
    '-TestExit=Automation Test Queue Empty' "-abslog=$log" `
    -unattended -nullrhi -nosound -nop4 -nosplash -ddc=InstalledNoZenLocalFallback
$editorExit = $LASTEXITCODE
Select-String -Path $log -Pattern 'Test Completed\.|TEST COMPLETE|LogAutomationController: Error'
if ($editorExit -ne 0 -or !(Select-String -Path $log -SimpleMatch 'Test Completed. Result={Success}' -Quiet)) {
    throw "Check the automation run in $log"
}
```

| Filter | Coverage |
| --- | --- |
| `AngelIO.Compiler.Contract` | Compilation, public contract, diagnostics and runtime failure cases |
| `AngelIO.Addressing.SpawnRenameDestroy` | Named group membership through spawn, rename and destruction |
| `AngelIO.Editor.MapContracts` | Map-level script, input and name diagnostics including player-controller missions |
| `AngelIO.Missions.` | Independent lift instances, overlapping typed operations and versioned checkpoint isolation |
| `AngelIO.Network.ListenServer` | `NamedGroupAndLateJoin`, `TypedTrips`, `MissionInspector`, `HandleLifetimeAndTimeout`, `StreamedTrips`, `WaitActor`, `MissionCheckpoints` using networked PIE |
| `AngelIO.Trace.` | Independent waiters, detachment, bounded history and long-running operations |

**Run the MCP integration.** This exercises an actual stdio client, reads project files and checks diagnostics for both valid and invalid scripts and maps. To include the trace assertions, use the basename of the listen-server export from the `TypedTrips` scenario as the extra argument `--trace <basename>` on the `node` command. That fixture contains six distinct calls. Pass `--timeout-trace <basename>` from `HandleLifetimeAndTimeout` to verify the poll, expired wait and final arrival for one operation ID. A manual ride or an inspector-only run has a different history. [Smoke test](../Tools/MCP/smoke.mjs)

```powershell
npm ci --prefix Tools/MCP
if ($LASTEXITCODE -ne 0) { throw 'MCP dependency install failed' }
node Tools/MCP/smoke.mjs --project $project --editor $editor
if ($LASTEXITCODE -ne 0) { throw 'MCP smoke test failed' }
```

### Diagnose a failed check

| Symptom | What to inspect |
| --- | --- |
| `FindLift` compiles but returns `null` | Check the authoritative world, the component's logical name, registration at `BeginPlay`, and whether two distinct lift actors have that name. |
| The client command produces no trace ID | Read the server RPC guards. The demo requires a floor from 1–8, finite speed greater than zero and at most 2,000 cm/s, and a panel currently waiting for `CallLift`. |
| `SendSignal` returns `false` | Check authority, the exact case of the signal, and the current wait. A mission blocked on an operation resumes from that operation's completion. |
| An external declaration is missing or the validator cannot produce a report | Check the `.uproject` and editor paths, rebuild `AngelIOHostEditor`, and verify provider registration in the commandlet process. |
| A script is found in PIE but missing from a packaged project | Check the script's project-relative location and UFS staging of `Content/AngelIO` in the consuming game. [Demo packaging settings](../Config/DefaultGame.ini) |

For a handoff, record the file or API changed, the tests actually run and the observable result. For a network investigation, include the host/client role, selected component, current source location, trace filename and operation ID. The issue and any durable follow-up belong in Beads. [Repository workflow](../AGENTS.md)
