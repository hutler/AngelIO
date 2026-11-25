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

The durable key uses the root map package, the level-instance package, the actor/component object path inside the level and the script path. PIE package prefixes are removed. Keep the same level-instance name when recreating a dynamic room. Two copies of a room have separate records. `TargetName` can change independently. A late client observes the restored mission's replicated outputs and waiting signal. [Working mission](../Content/AngelIO/Examples/RoomDelivery.as) · [Listen-server scenarios](../Source/AngelIOHost/Tests/AngelIOListenServerTests.cpp)

The authority opens its SQLite slot on demand. The default name is `Default`. Use the host map URL option `?AngelIOCheckpointSlot=Campaign2` to select another slot, or an empty value for an in-memory session. Named slots accept up to 64 letters, digits, underscores or hyphens. Look under `Saved/AngelIO/Checkpoints/Game` in a game and `Saved/AngelIO/Checkpoints/PIE` in PIE. Save commits the stage, version, checkpoint ID and writer instance in one transaction. I/O errors fail the script with a diagnostic. A restart with the same slot loads the committed stage and preserves the ID used by the inspector and `read_trace`. Each server session has its own trace export.

The demo lift stores its last completed floor and vertical offset under its placed mission component's identity in the same slot. The game module writes this record before signaling arrival. During recovery the host positions the cab, seeds the executor's `StartingFloor`, and skips the startup request. Clients receive the floor and movement through replication. The new executor starts with `TotalTrips = 0` and `TripSpeed = 0`. A mid-ride unload preserves the prior landing. Disable **Persist Landing** on a placed lift to use its authored startup behavior on every load. [Demo lift](../Source/AngelIOHost/AngelIOExampleLift.cpp)

## Listen-server investigation

Run **Play As Listen Server** with **2 players** and **Run Under One Process** on `/Game/Demo/TwoLifts`. The host runs both players' missions. Each player owns a separate `TripPanel` on their controller. Client input travels through the owning controller's server RPC. `GetOutput` and `GetWaitingSignal` on the client read the latest replicated public state. Use `OnPublicStateChanged` for persistent UI. Opted-in `OnPresentationSignal` effects use unreliable multicast and can arrive before state updates. Late joiners receive current state. [Controller and RPC checks](../Source/AngelIOHost/AngelIOPlayerController.cpp) · [Replication and callbacks](../Plugins/AngelIO/Source/AngelIO/Private/AngelIOScriptComponent.cpp)

1. Start with a fresh slot and wait for the two startup rides to finish. In the connected client's console enter `LiftTrip 6 100`. Open **Tools → AngelIO Mission Inspector**, select **Listen server**, then the remote player's **TripPanel**. Read the current source line, wait label (`LiftTrip.Wait`), and full operation ID.
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
| `describe_scene` | `{"map":"/Game/Demo/TwoLifts"}` | Actor and component IDs, owning room, mission path, initial overrides, local target bindings and diagnostics |
| `write_script` | `script`, `source`, `expectedRevision`, optional `dryRun` | Read the SHA-256 revision from `read_script`. Use null to create a file. Dry-run first, then submit with `dryRun: false`. A stale revision returns `revision_conflict`; a concurrent publisher can return `write_busy`. Re-read before retrying. |
| `read_trace` | Optional fields are `trace` (export basename), one of `operationId`, `checkpointId`, or `stateId` (hyphenated GUID), `afterSequence` (exclusive, default 0), `limit` (1–200, default 50) | `trace`, `schemaVersion`, `sessionId`, `world`, `netMode`, `droppedEvents`, `events`, `nextAfterSequence` |

**Editing loop.** Call `list_scripts`, `describe_api`, and `read_script`. Edit the file with the workspace editor, then send its relative path to `validate_script`. A compiler error is a successful tool response with `valid: false`. Read each diagnostic's `severity`, `message`, `line` and `column`. A path or tool execution error sets `isError`. A valid report confirms compilation and public declarations. Use PIE for actor lookup, movement, replication and completion.

**Map checks.** Use **Tools → Validate AngelIO Missions** for the current editor world, or `validate_map` for a saved package. Each distinct script compiles once per check. `input_unknown` and `input_type` identify the component's initial override. `target_missing` and `target_ambiguous` identify a direct literal lookup and its source line. Providers can register global lookup names, actor types and the required cardinality through `GetMapLookups`. Use a unique global function name for each addressing rule. The static check recognizes unqualified calls with a plain double-quoted first argument. Escaped strings, expressions and names shadowed by a script function or method are skipped. The validator examines loaded placed actors and the configured player controller's mission templates. Exercise computed names and streamed or spawned targets in PIE.

**Trace lookup.** Call `read_trace` with `{}` for the latest snapshot, or pass the basename from the host log. Preserve the returned `trace` for every subsequent call. Find the `call` with the desired `operation`, `target`, `arguments` and `caller`. Pass its `operationId` and the same `trace` to read the ride. If `nextAfterSequence` is a number, pass it as `afterSequence` with the same filters and repeat until it is `null`. The filename pins the snapshot even when another PIE world exports during paging.

**Checkpoint lookup.** After a room reloads, select the recreated component in the inspector. Copy its checkpoint ID and call `read_trace` with the same `trace` and that `checkpointId`. For `RoomDelivery.as`, after interrupting the first attempt, expect `checkpointSaved(Dispatched) → checkpointRestored(Dispatched) → checkpointSaved(Delivered) → checkpointRestored(Delivered) → checkpointSaved(Complete) → checkpointRestored(Complete)` across the room reloads. Each event includes the stage, version, writer's `savedByInstanceId`, and the current `caller` with its component, instance ID, script and line. The first successful load from a previous VM produces one restore event per run. Empty reads, version mismatches and reads of the VM's own writes leave the restore point unchanged. The inspector reads the saved record and the current VM directly, so their stages remain visible when older trace events expire. New exports use schema version 3. The tool also accepts version 1 and 2 histories.

**Lift landing lookup.** Select **FreightLift / Mission** in the PIE inspector. The saved floor updates after the server commits an arrival. The restored floor stays at the value applied at the start of this actor activation. The inspector retains both values independently of trace retention. In the client PIE world, selecting the replica resolves the same authoritative values when its server is available in the editor.

Pass the inspector's `stateId` to `read_trace` first with the old host's export and then with the new host's export. Each result carries the target name, component path, integer field and value. Look for `stateSaved` on the first host and `stateRestored` on the second. The stable ID identifies a record in its slot, root map, room instance and component. The filtered sequences and pagination cursors belong to their own exports. For a stdio regression against both processes, pass `--landing-write-trace <first-basename> --landing-read-trace <second-basename>` to the smoke test.

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
| `AngelIO.Network.ListenServer` | `NamedGroupAndLateJoin`, `TypedTrips`, `MissionInspector`, `SharedRegistrationInspector`, `HandleLifetimeAndTimeout`, `StreamedTrips`, `WaitActor`, `MissionCheckpoints`, `CheckpointRestart`, `MidRideRestart`, `LandedBeforeDeliveredRestart`, `BlinkBranches` and `PartitionLanding` using networked PIE |
| `AngelIO.Performance.WaitingMissions` | Startup, process resident-memory growth and baseline/loaded frame time for 1, 100 and 1000 independently ticking mission components |
| `AngelIO.Performance.MissionStartup` | Paired startup measurements for 1000 instances of the same script, with per-phase timings and a bytecode reuse experiment |
| `AngelIO.Performance.SharedRegistration` | Paired source builds with one registered engine per mission or per batch of 1000 missions |
| `AngelIO.Missions.SharedRegistrationIsolation` | Independent globals, signals, wait locations and compiler diagnostics on a shared engine |
| `AngelIO.Trace.` | Independent waiters, detachment, bounded history and long-running operations |

To verify a process restart, run the automation command above twice with the filter `AngelIO.Network.ListenServer.CheckpointRestart`. Generate a fresh GUID with `[guid]::NewGuid()`. Add `-AngelIORestartTestWrite=<guid>` to the first editor invocation and `-AngelIORestartTestRead=<guid>` to the second. The first host delivers to floor 4 and exits. The second host restores `Delivered` and accepts confirmation. The test checks floor 4, the physical cab position and zero new trips on both peers, and zero delivery calls in the new host's trace. Successful completion removes the isolated test slot.

Use the same two commands and a fresh GUID with the filter `AngelIO.Network.ListenServer.MidRideRestart` to stop the first host during the ride from floor 2 to floor 4. The writer waits for both cabs to move off the landing. The reader starts the mission from `Dispatched`, observes its physical departure from floor 2 and checks one delivery call, one arrival and the final replicated floor and position. Only the writer sends `Dispatch`.

Use `AngelIO.Network.ListenServer.LandedBeforeDeliveredRestart` with a fresh GUID to exercise the gap between the landing commit and the mission commit. The writer suspends the delivery component's tick after dispatch, lets the lift finish, and exits with floor 4 and `Dispatched` on disk. The reader must show `stateRestored(4)` for the freight lift, `checkpointRestored(Dispatched)`, and `checkpointSaved(Delivered)`. The host and client wait for `Confirm`. The reader trace must have zero `Lift.MoveTo` calls and zero `Arrived` results.

The example's `Lift.HasSavedArrival(floor)` checks the last committed landing against the caller's checkpoint step. The provider captures `AngelIO::GetCheckpointStepId()` at `MoveTo` and commits it with the floor and cab offset in one SQLite write. Loading the matching checkpoint reproduces the step ID from the checkpoint writer, version and stage. Use a distinct checkpoint stage for each delivery within one mission run. Another mission's ride, an untracked landing, or an older landing payload without a step ID cannot acknowledge a pending delivery.

**Run the MCP integration.** This exercises an actual stdio client, reads project files and checks diagnostics for both valid and invalid scripts and maps. To include the trace assertions, use the basename of the listen-server export from the `TypedTrips` scenario as the extra argument `--trace <basename>` on the `node` command. That fixture contains six distinct calls. Pass `--timeout-trace <basename>` from `HandleLifetimeAndTimeout` to verify the poll, expired wait and final arrival for one operation ID. A manual ride or an inspector-only run has a different history. [Smoke test](../Tools/MCP/smoke.mjs)

Pass `--checkpoint-trace <basename>` from the `Checkpoint trace` line in the `MissionCheckpoints` test log to verify checkpoint filtering, pagination, stages, source lines and writer/reader identities.

```powershell
npm ci --prefix Tools/MCP
if ($LASTEXITCODE -ne 0) { throw 'MCP dependency install failed' }
node Tools/MCP/smoke.mjs --project $project --editor $editor
if ($LASTEXITCODE -ne 0) { throw 'MCP smoke test failed' }
```

### Win64 release and scaling checks

`AngelIO.Performance.WaitingMissions` starts independent actors, each with a component, compiled VM and pending game-time wait. It measures synchronous spawn and start, change in process resident memory, and mean game-thread frame time across 120 fixed-delta test-world ticks. The baseline uses the same world before spawning. One Development Editor run on Windows x64 with NullRHI and 12 physical cores produced these values.

| Waiting missions | Total startup (ms) | Resident growth (MiB) | Empty frame (ms) | Waiting frame (ms) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.66 | 0.00 | 0.066 | 0.063 |
| 100 | 48.41 | 7.65 | 0.066 | 0.066 |
| 1000 | 484.10 | 87.39 | 0.073 | 0.395 |

These figures include actor creation and editor tracing. The memory delta reflects process allocation and reuse. With 1000 waiting missions the additional tick cost was about 0.32 ms. Keep the current tick model for this workload.

#### Where startup time goes

`AngelIO.Performance.MissionStartup` starts 1000 actors running `Examples/TimedGate.as` through component registration and `BeginPlay`. Each mission gets its own AngelScript engine, module, globals and execution context. The source cohort builds every module. In the bytecode cohort the first mission builds and saves the module with debug information (471 bytes in this run), then the other 999 load those bytes from memory. Both cohorts read the file, register the same native API and inspect the mission contract on every start.

These are arithmetic means in milliseconds per 1000 starts over six paired runs on UE 5.8.4 Win64 Development Editor, NullRHI, Ryzen 9 7845HX. The test warms both paths on eight instances each, creates a fresh game world for each cohort, alternates which cohort runs first and excludes world creation, teardown and assertions. Running the original `WaitingMissions` test in the same editor session gave 497.3 ms, compared with 484.1 ms in the earlier release.

| Synchronous work for 1000 missions | Build from source (ms) | Reuse bytecode (ms) |
| --- | ---: | ---: |
| Spawn actors | 29.5 | 28.6 |
| Resolve the script path and read the file | 52.5 | 51.4 |
| Create the AngelScript engines and program storage | 40.0 | 39.9 |
| Register the standard string type and AngelIO core API | 154.8 | 153.8 |
| Register external providers and collect their API declarations | 70.6 | 69.9 |
| Prepare the module and source sections, including wrappers | 14.4 | 14.7 |
| Build or load the module | 88.9 | 36.1 |
| Inspect the contract and editor metadata | 6.7 | 5.9 |
| Initialize the VM, apply inputs and run to the first wait | 34.5 | 33.5 |
| Component creation, registration, state publication and loop overhead | 15.2 | 15.0 |
| **Total** | **507.2** | **448.6** |

On the source path, `CompileFile` accounted for 428.0 ms including file access. The SDK's `Build()` itself accounted for 88.9 ms. Reusing bytecode saved 52.8 ms inside the module step and 58.6 ms end to end (11.6%, with savings of 54.5–63.2 ms across pairs). The largest cost was repeating core and provider registration and collecting declarations, at 225.5 ms. Defer a persistent bytecode cache for this short mission.

To reproduce, use the Unreal Automation command above with `$filter = 'AngelIO.Performance.'`. Each `AngelIO startup pair=` log entry includes the phase timings, build and load counts, and bytecode size. The test checks the waiting state and initial output of every instance and verifies that the first and last retain separate mission IDs and the same valid source line. The bytecode path exists only in editor automation.

#### Registering the API once for 1000 missions

`AngelIO.Performance.SharedRegistration` compares two source builds of the same `TimedGate.as` workload in the same editor process. Each measured cohort pays for its first registration. In the shared cohort, each mission creates a uniquely named module and its own context on one engine. Every cohort performs 1000 file reads, 1000 source builds and zero bytecode loads. The game module and its native providers stay loaded through teardown. Engine sharing is enabled only inside editor automation.

On the same Win64 machine, checking out tag `v0.6.2` and rerunning its source benchmark gave **520.5 ms** (published mean **507.2 ms**). The table reports the six alternating pairs from the new editor run. Both paths used the same timers and exact-line checks. The sums below group the individual phase timings from the log.

| Work per 1000 starts | Private engines (ms) | One registered engine (ms) |
| --- | ---: | ---: |
| Spawn actors | 28.4 | 27.8 |
| Resolve and read the script | 49.2 | 47.4 |
| Acquire the engine and prepare per-mission storage | 39.4 | 1.4 |
| Register or reuse the core and external API, including declaration metadata | 217.0 | 1.6 |
| Create modules and prepare source sections | 13.9 | 23.5 |
| Compile the 1000 modules from source | 86.1 | 83.3 |
| Inspect the contract and editor metadata | 6.6 | 6.4 |
| Initialize the VM and run to the first wait | 33.0 | 31.7 |
| Component and loop overhead | 15.3 | 14.4 |
| **Total** | **488.9** | **237.6** |

One registration saved **251.4 ms (51.4%)** on average. Every paired run saved time, from 243.3 to 256.1 ms. This result supports testing a world-owned engine for runtime use. Streaming, world teardown and provider lifetime in a packaged game are the next acceptance cases.

To reproduce the timing in isolation, set `$filter = 'AngelIO.Performance.SharedRegistration'` in the automation command above. Two separate tests cover behavior outside the timed loop.

- `AngelIO.Missions.SharedRegistrationIsolation` starts 1000 instances of `Elevator.as` with distinct starting floors. It leaves one group idle, holds one in motion and completes one trip in each remaining instance. It checks every floor, trip count, wait, mission ID and source line (18 or 25), and checks diagnostic ownership after a sibling fails to compile.
- `AngelIO.Network.ListenServer.SharedRegistrationInspector` restarts both player panels on one engine. The live inspector selects the client replica at `TypedLift.as:22` and the host panel at `TypedLift.as:11`. The host handles a rejected call and stops. The surviving panel completes its own ride, replicates floor 4 to the client and returns to line 11. The last panel releases the engine.

The inspector now records the executing line when a native or signal wait begins. For example, an active timer in `TimedGate.as` shows line 6, even though the suspended SDK context has already advanced to line 7.

In `Examples/BlinkPanel.as`, `Notify(RideEnded)` queues the signal that stops the blinking branch. The parent waits for its delivery before switching off the lamp. The listen-server regression holds the panel until the lift arrival and blink timer are both ready, then verifies the client sees the lamp off and the pulse counter stays fixed.

`AngelIO.Network.ListenServer.PartitionLanding` uses the small partitioned map `/Game/Tests/PartitionLift`. The host and client move close enough to load the freight lift, ride to floor 4, leave its cell, and return. The test checks removal from the registry, restored physical height and zero new trips. The read half of `LandedBeforeDeliveredRestart` also connects a late client after the new server is already waiting for confirmation.

To reproduce the packaged checks, set `$engine` to the Unreal installation directory and run from the checkout. The staged games use cooked maps and IoStore. `-SkipZenStore` selects local cooked output for this run.

```powershell
$project = (Resolve-Path AngelIO.uproject).Path
foreach ($config in 'Development', 'Shipping') {
    $stage = Join-Path $PWD "Saved/AngelIO/Packages/0.6.1/$config"
    & "$engine/Engine/Build/BatchFiles/RunUAT.bat" BuildCookRun "-project=$project" -target=AngelIOHost -platform=Win64 "-clientconfig=$config" -build -cook -stage -pak -iostore -package '-map=/Game/Demo/TwoLifts+/Game/Tests/PartitionLift' "-stagingdirectory=$stage" -AdditionalCookerOptions=-SkipZenStore -ddc=InstalledNoZenLocalFallback -nop4 -unattended
    if ($LASTEXITCODE) { throw "Packaging failed for $config" }
    $game = if ($config -eq 'Shipping') { 'AngelIOHost-Win64-Shipping.exe' } else { 'AngelIOHost.exe' }
    & "$engine/Engine/Binaries/ThirdParty/Python3/Win64/python.exe" Tools/TestPackagedLandings.py "$stage/Windows/AngelIO/Binaries/Win64/$game"
    if ($LASTEXITCODE) { throw "Packaged landing check failed for $config" }
}
```

The probe starts each packaged executable with a temporary user directory and opens its SQLite slot read-only. It exits successfully once both lifts commit their expected floors, 2 and 3.

### Diagnose a failed check

| Symptom | What to inspect |
| --- | --- |
| `FindLift` compiles but returns `null` | Check the authoritative world, the component's logical name, registration at `BeginPlay`, and whether two distinct lift actors have that name. |
| The client command produces no trace ID | Read the server RPC guards. The demo requires a floor from 1–8, finite speed greater than zero and at most 2,000 cm/s, and a panel currently waiting for `CallLift`. |
| `SendSignal` returns `false` | Check authority, the exact case of the signal, and the current wait. A mission blocked on an operation resumes from that operation's completion. |
| An external declaration is missing or the validator cannot produce a report | Check the `.uproject` and editor paths, rebuild `AngelIOHostEditor`, and verify provider registration in the commandlet process. |
| A script is found in PIE but missing from a packaged project | Check the script's project-relative location and UFS staging of `Content/AngelIO` in the consuming game. [Demo packaging settings](../Config/DefaultGame.ini) |

For a handoff, record the file or API changed, the tests actually run and the observable result. For a network investigation, include the host/client role, selected component, current source location, trace filename and operation ID. The issue and any durable follow-up belong in Beads. [Repository workflow](../AGENTS.md)
