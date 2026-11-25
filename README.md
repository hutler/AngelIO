# AngelIO

AngelIO is an Unreal Engine plugin for writing level logic in AngelScript. A mission can find level actors by name, call typed functions supplied by game modules, and wait for a particular operation to finish. The editor and the packaged game use the same runtime to compile UTF-8 `.as` files from `Content/AngelIO`. The included demo targets Unreal Engine 5.8.4 on Windows x64.

For LLM agents and contributors, the [agent guide](Docs/AgentGuide.md) walks through a mission change, a native async operation, and a listen-server investigation with MCP and the PIE inspector. [Repository workflow](AGENTS.md)

## Illustrated user handbooks

| Handbook | Download PDF (English) | Скачать PDF (русский) |
| --- | --- | --- |
| Engineers · Build the plugin, expose a typed operation, and trace a networked ride | [Engineering handbook](Docs/Guides/Engineering-EN.pdf?raw=1) | [Для инженеров](Docs/Guides/Engineering-RU.pdf?raw=1) |
| Level designers · Tune the two-lift map, wire a Blueprint button, and playtest with a client | [Level design handbook](Docs/Guides/Level-Design-EN.pdf?raw=1) | [Для левел-дизайнеров](Docs/Guides/Level-Design-RU.pdf?raw=1) |

Open the downloaded PDF to follow its links. You can also open the script from page 5 here. [Platform movement script / Скрипт движения платформы (Elevator.as)](Content/AngelIO/Elevator.as)

## Run the two-lift demo

Open [AngelIO.uproject](AngelIO.uproject) with Unreal Engine 5.8, build the C++ modules if prompted, and press **Play** on the default map, `/Game/Demo/TwoLifts`. Each platform runs its own instance of `Elevator.as`. In a fresh save slot the lifts start on floor 1 and travel to floors 2 and 3. On later runs they start at their last completed landings. Each floor is 200 cm apart and the default speed is 240 cm/s.

Once the platforms stop, try either command in the player console. Both work from the listen-server host and from a connected player.

| Command | What to expect |
| --- | --- |
| `LiftTrip 6 400` | The freight lift moves to floor 6 at 400 cm/s. The requesting player's `TripPanel` receives the result for that ride. |
| `LiftFloor 4` | The panel sends `RequestFloor` to the `DemoLifts` group. Both idle lifts move to floor 4. |

To change the initial ride, select a placed `AngelIOExampleLift` and edit **Requested Floor**. The `Mission` component's **Initial Inputs** accepts `StartingFloor` (int), `TravelSpeed` (float, in cm/s), and `Enabled` (bool). A Blueprint can request another ride with **Request Floor**.

## Call a typed API and wait for a trip

Select a placed script component and choose a mission from the **Script** dropdown. **Mission Contract** shows each compiled input and output, its type and its source initializer. Enter an input override or clear it to use the script default. For a literal target, choose a named endpoint in the room to bind that role to the current level instance. The same panel can then drive different lifts in two copies of a room. **Tools → Validate AngelIO Missions** checks missing and ambiguous bindings before PIE.

The demo game module exposes `Lift`, `LiftTrip`, and `TripResult` to AngelScript. This mission records the floor only after its own ride arrives.

```angelscript
int out_ArrivedFloor;

void Run()
{
    Lift@ lift = FindLift("FreightLift");
    if (lift is null) return;

    LiftTrip@ trip = lift.MoveTo(6, 400.0f);
    TripResult@ result = trip.Wait();
    if (result.Status == TripStatus::Arrived)
        out_ArrivedFloor = result.Floor;
}
```

`FindLift` looks for a single matching lift actor in the current mission's world. A missing or ambiguous name returns `null`. On the demo map, the lifts' `Address` components use the individual names `FreightLift` and `ServiceLiftB`. Their `Mission` components share the group name `DemoLifts`.

Each `MoveTo` call returns its own trip handle. A script can start multiple trips, keep the handles, and wait for them in any order. Waiting for a completed trip returns immediately, including a request for the floor where an idle lift already stands. The result stays available while the script holds a reference. `result.Floor` always contains the requested floor.

| `result.Status` | Meaning |
| --- | --- |
| `TripStatus::Arrived` | The platform reached the requested floor. |
| `TripStatus::Rejected` | The lift is busy, disabled, or unready, the floor is outside 1–8, or the speed is not a finite positive number. |
| `TripStatus::Cancelled` | The lift left play, its mission stopped during the ride, or the host could not save the landing (`LandingSaveFailed` in the trace). |
| `TripStatus::TimedOut` | This reader's deadline expired while the ride was still pending. The lift continues moving. |

`trip.Id` and `result.Id` return the same operation ID as a string. Use it to look up the ride in an exported trace.

Use `trip.Wait(2.0f)` to wait up to two seconds of game time. The script can react to `TimedOut` and call `trip.Wait()` later on the same handle. Each wait retains its own result. `Wait(0)` polls immediately. If the ride has already finished when the VM checks, either overload returns its terminal result. Supply a finite, non-negative timeout.

The playable version takes the floor and speed from the player's command. It publishes `LastStatus`, `LastFloor`, and `Completed` on `TripPanel` for the owning player. [Demo mission](Content/AngelIO/TypedLift.as)

## Address actors by name

`Fork(Blink)` starts a child function with its own execution context and shared mission globals. A child can call `EndOn(InputSignal::RideEnded)` and `WaitSeconds(0.1f)` to blink a panel while its parent awaits a ride. The parent calls `Notify(InputSignal::RideEnded)` at arrival. Notifications are delivered after the current script slice. Cancellation takes precedence over a matching signal wait in that branch. Returning from a function started with `Fork` ends its descendants. Ending the mission or unloading the room releases all remaining branches. The PIE inspector lists each child function, source line and wait. [Blinking panel mission](Content/AngelIO/Examples/BlinkPanel.as)

A successful `FindLift("FreightLift")` binds its handle to that particular actor. Renaming the endpoint changes subsequent lookups. The stored handle still calls the original lift. After its destruction, calling `MoveTo` on the stored handle yields `Cancelled` with trace reason `TargetGone`. Look up the name again to use a replacement.

### Stream a room containing a lift

In a persistent mission, wait for the named lift before starting a ride:

```angelscript
Lift@ lift = WaitActor("FreightLift", 5.0f);
if (lift is null)
    return;
TripResult@ ride = lift.MoveTo(6, 800.0f).Wait();
```

`WaitActor` returns when exactly one matching lift has begun play and its mission is ready to accept a floor request. A startup ride can still be in progress when the room appears. Missing, ambiguous and busy lifts keep the caller waiting. The optional timeout is in game seconds. Expiry returns `null`, zero polls immediately, and omitting the argument waits until ready or until the mission stops. A new call after unloading resolves the current lift. The lookup and the subsequent trip have separate trace IDs and deadlines. [Playable controller mission](Content/AngelIO/Examples/StreamedLift.as)

When streaming removes the lift from play during a ride, the persistent caller receives `Cancelled` and the trace records `TargetUnloaded`. Its cached `Lift@` expires at that boundary. Showing the same room again requires a fresh lookup even if Unreal reuses the actor in memory. A mission attached to the streamed room itself ends with its component and starts from `Run` on re-entry when **Start On Begin Play** is enabled.

With **Persist Landing** enabled (the default), each demo lift saves its floor and cab height relative to its placement as soon as it arrives. After a room reload or a server restart using the same slot, the host places the cab at its last landing and replicates its position and `LastFloor` to clients. The lift waits for a new request. Interrupting a ride leaves the previous landing intact.

Set **Target Name** on an Angel IO Script Component in the Details panel. This logical name is independent of the actor's Outliner label. A shared name lets a mission send a signal to a group.

```angelscript
SendTo("DemoLifts", "RequestFloor", 4);
```

`SendTo` queues the request alongside `Emit`. When execution reaches a wait or returns, the registry selects live components with that name and delivers `RequestFloor` with the integer payload `4` to each one. A mission waiting for that signal resumes. Blueprint and C++ endpoints can also handle `OnNamedInput` directly. Disable **Start On Begin Play** on an endpoint that has no script.

Each world has its own registry. Components register at `BeginPlay` and unregister at `EndPlay`. Use `SetTargetName` to rename a component during play. A newly spawned component joins future sends. Target names follow Unreal's case-insensitive `FName` rules. Signal names are case-sensitive. An empty target name leaves the component outside all groups. Blueprint and C++ can call `FindTargets` and `SendTo` on the `AngelIOTargetRegistry` world subsystem.

## Write a mission

1. Save a script under `Content/AngelIO`, for example `Missions/Arrival.as`. Add an **Angel IO Script Component** to an actor and set **Script** to `Missions/Arrival.as`.
2. Declare inputs as `in_Name` globals and outputs as `out_Name` globals. Supported port types are `int`, `float`, and `bool`. The component uses the case-sensitive name without the prefix. Each instance owns its globals.
3. Declare incoming and outgoing events in the global `InputSignal` and `OutputSignal` enums. Give each member a unique value within its enum. Each signal carries one `int`.
4. Define `void Run()` as the entry point. Use `WaitFor` and `SignalValue()` to receive an input, and `Emit` to send an output event.

```angelscript
int in_Bonus = 2;
int out_Score;
enum InputSignal { Award }
enum OutputSignal { ScoreChanged }

void Run()
{
    WaitFor(InputSignal::Award);
    out_Score = SignalValue() + in_Bonus;
    Emit(OutputSignal::ScoreChanged, out_Score);
}
```

Calling `SendSignal("Award", 3)` on the waiting component emits `ScoreChanged` with value `5`. Subscribers receive the event through `OnSignal`. The `Score` output stays readable after the mission returns. `GetOutput` and `SetInput` return a success flag. Blueprints can construct and inspect an **Angel IO Value** with the standard Make and Break nodes.

Scripts start at `BeginPlay` by default. To observe the first event in Blueprint, disable **Start On Begin Play**, bind `OnSignal`, then call `StartScript`. Starting again reads the file and creates a fresh instance. Restarting Play In Editor (PIE) also picks up edits.

### Signal delivery and lifetime

- Each instance runs one execution path. `SendSignal` returns `false` when the instance is not waiting for that signal.
- `Emit` queues events in order. The component dispatches them once the script suspends or returns, so a subscriber can answer synchronously in the same frame and resume the next wait.
- An exception discards the current batch of outgoing events and reports diagnostics to `LogAngelIO`. `GetWaitingSignal` shows what the instance is waiting for.
- The component owns the execution context. `StopScript` and `EndPlay` release it and clear its wait. Call the component from the game thread.

### Resume a mission when its room returns

Call `SaveCheckpoint("Delivered", 1)` after the delivery succeeds. On the next activation, the component starts a fresh `Run()`. Read `LoadCheckpoint(1)` there and enter the matching stage. The saved name survives unloading, and the new VM sets up its own signal waits. The default version is `1`. A missing record or a different version returns an empty string.

In the two-lift map, add a script component to a replicated actor in the streamed room and assign `Examples/RoomDelivery.as`. Send `Dispatch` once on the server. The script commits `Dispatched` before requesting floor 4. If the server restarts mid-ride, the cab returns to its last completed landing and the mission automatically issues a fresh `MoveTo` for floor 4. After arrival the mission commits `Delivered`, publishes `out_Stage = 1` and waits for `Confirm`. Confirmation commits `Complete` and publishes stage 2. [Delivery mission](Content/AngelIO/Examples/RoomDelivery.as)

The host commits each stage to disk before `SaveCheckpoint` returns, using Unreal's bundled SQLiteCore plugin. Restart the listen server on the same map and slot, then load the room with the same instance name. If the mission had already committed `Delivered`, the freight lift is on floor 4. `RoomDelivery.as` resumes at `Delivered` and waits for `Confirm`, or finishes immediately if it already saved `Complete`. In older slots that contain only mission checkpoints, each lift uses its authored starting configuration until it first saves a landing.

If the server stops after the freight lift saves floor 4 and before the mission saves `Delivered`, the new host still loads `Dispatched`. The landing record identifies the mission step that requested the trip. When `lift.HasSavedArrival(4)` matches, the mission commits `Delivered` and waits for `Confirm`. The client sees the restored fourth-floor cab with zero new trips. Earlier landing records still restore the cab's position. A pending dispatch with no matching receipt requests its ride again.

The default slot is `Default`. To start a separate campaign, open the host map with a different slot, for example `/Game/Demo/TwoLifts?listen?AngelIOCheckpointSlot=Campaign2`. The host stores it under `Saved/AngelIO/Checkpoints/Game/slot-Campaign2.sqlite3`. PIE uses the sibling `PIE` directory. For a session-only playtest, use an explicitly empty `?AngelIOCheckpointSlot=`. Keep the root map, level-instance name, actor/component object name and script path stable. Bump the checkpoint version when old stages become incompatible.

After a reload, select the room's new component in **Tools → AngelIO Mission Inspector**. Under **Mission checkpoint**, **Saved stage** shows the latest commit and **Restored this run** shows the stage read from a previous run. After confirming the example delivery, these read `Complete (v1)` and `Delivered (v1)` respectively. Copy the checkpoint ID and pass it to `read_trace` as `checkpointId` to see the writes and restores in order.

To inspect the cab itself, select **FreightLift / Mission**. Under **Persisted component state**, **Saved floor** shows its latest committed landing. **Restored floor** shows the floor applied when this lift entered play. A fresh lift shows **None this activation** until a later activation restores a landing. If the cab restores at 4 and then arrives at 6, the two values read 6 and 4.

Copy the **State ID** and call `read_trace` with `trace` and `stateId` for each host export. In the delivery example the last freight event before shutdown is `stateSaved` with `field="floor", value=4`. The new host records `stateRestored` with the same ID and value. Each lift has its own ID in the slot.

## Use the plugin in your project

Copy [Plugins/AngelIO](Plugins/AngelIO) into a C++ project and enable the plugin. Put mission files in `Content/AngelIO`. Add `AngelIO` to **Project Settings → Packaging → Additional Non-Asset Directories to Package** so newly added scripts ship with the game. The demo already includes this setting. [Demo packaging configuration](Config/DefaultGame.ini)

Add `AngelIO` to the dependencies in your module's `Build.cs` to use the component or register script bindings. The plugin ships with the AngelScript 2.39.0 static library for Win64. The rebuild script verifies the SDK archive's SHA-256 and uses MSVC 2022 x64. Other platforms require a library build for that target. [Rebuild script](Tools/BuildAngelScript.ps1) · [AngelScript SDK](https://www.angelcode.com/angelscript/downloads.html)

### Configure the mission bytecode cache

Each authority world caches compiled mission bytecode across room unloads. The default budget is **256 KiB per world**. Set **Project Settings → Plugins → AngelIO → Bytecode Cache Budget (KiB)** or override a single world with `?AngelIOBytecodeCacheKiB=32` in the listen or travel URL. Zero disables the cache. When space runs out the world evicts the least recently used entry. A script larger than the limit still runs from source. [Configuration, runtime counters and listen-server results](Docs/AgentGuide.md#runtime-bytecode-cache)

### Expose types from a game module

Implement `IAngelIOScriptAPI::RegisterBindings(asIScriptEngine&)` and register the provider with `IModularFeatures` during `StartupModule`. The first runtime mission in each authority world registers the API. Every mission compiles into a separate module with its own globals, context and waits. Unloading a streamed room releases its modules. Reloading the room reuses the world's registered API. A replacement world registers its own engine on first use. Client replicas read replicated state. [World lifetime and native callers](Docs/AgentGuide.md#world-owned-angelscript-engine)

Keep providers loaded through teardown of the worlds and validation programs using their bindings. Unregister them in `ShutdownModule`. The editor validator and MCP commandlets compile on private engines with the providers loaded in their process. [Provider interface](Plugins/AngelIO/Source/AngelIO/Public/AngelIOScriptAPI.h) · [Demo bindings](Source/AngelIOHost/AngelIOHost.cpp)

Inside a native binding, `AngelIO::GetScriptWorld()` gives access to the current mission's world. For a long-running action, store the result in a type derived from `FAngelIOOperation`. Write the result and call `MarkComplete()` on the game thread. Pass the same operation to `AngelIO::AwaitOperation` when the script waits. The component checks completion on its tick while that wait is active. Stopping the caller releases its wait. The lift continues to own any ride it has already started. [Lift implementation](Source/AngelIOHost/AngelIOExampleLift.cpp)

Every `FAngelIOOperation` gets a unique `FGuid`, available through `GetOperationId()`. To describe it in the trace, pass the world, operation name, logical target, and optional JSON arguments to the base constructor. A binding running inside a mission can use its current script world. A native producer outside a script should pass its world explicitly. Call `MarkComplete(Status, Reason)` after recording the domain result. AngelIO captures the active caller and each waiter separately, including when they belong to different script instances. [Lift operation](Source/AngelIOHost/AngelIOLiftTrip.cpp)

### Multiplayer

The server runs the missions and resolves named targets. Enable **Replicates** on the owning actor to publish component state. On a client, `GetOutput` and `GetWaitingSignal` read the latest replicated values. Subscribe to `OnPublicStateChanged` to update UI, including for late joiners. Inputs and the execution stack stay on the server. Intermediate changes can coalesce between network updates.

`OnSignal` drives authoritative gameplay. For a sound or visual effect, add the output event name to **Presentation Signals** and subscribe to `OnPresentationSignal`. The host gets one local notification. Relevant clients receive an unreliable multicast carrying its own payload. An effect may arrive before the output update or be lost. A late joiner receives the current outputs without replaying earlier effects.

In the demo, the player's controller sends the request to the server. `LiftFloor 4` and the Blueprint call `ServerRequestLiftFloor(4)` both reach the group through `LiftPanel.as`. The typed `LiftTrip` console command accepts floors 1–8 and speeds up to 2000 cm/s. Each game's module controls which commands and arguments players may send. [Demo controller](Source/AngelIOHost/AngelIOPlayerController.cpp)

## Check a map before Play

Open **Tools → Validate AngelIO Missions**. The Map Check log identifies the component, field, script and available source line for each problem. It checks mission files, initial input names and types, and names used directly in calls such as `FindLift("FreightLift")` and `SendTo("DemoLifts", ...)`. A typed lift lookup needs one matching lift actor. A group can contain multiple actors. A named endpoint with no script can receive events when **Start On Begin Play** is off.

The editor checks the open map, including unsaved edits. MCP checks the saved package with `validate_map({"map":"/Game/Demo/TwoLifts"})`. Both inspect loaded level actors and missions on the default player controller. Targets spawned during play, unloaded streaming cells and names computed by scripts need a runtime check in PIE.

## Inspect a mission during PIE

Open **Tools → AngelIO Mission Inspector** in the Unreal Editor. Start the demo with **Play As Listen Server**, two players and **Run Under One Process**. Use a fresh slot for this walkthrough. Wait for the initial rides to finish, then enter `LiftTrip 6 100` in the client's console.

1. Select the **Listen server** world and the remote player's **TripPanel** mission. The inspector shows the source file and line of the current wait, its signal or operation name, and the ID of the awaited ride.
2. Select **FreightLift / Mission**. Its script waits for `MotionFinished` in `Elevator.as`. The operation history follows the same ride that the panel is waiting for. The list includes call arguments, caller and waiter sites, completion status, rejection or cancellation reason, and resumption.
3. Use the operation picker to pin an earlier ride. Its history remains visible as the live mission advances. Choose **Follow current or latest operation** to resume automatic selection.

The mission picker lists scripted components in the selected world, including multiple panels on one controller. It groups missions on the selected Outliner actor when its counterpart is available. Selecting a client replica shows its replicated wait alongside the live server VM when the editor can resolve the replica through the in-process IP NetDriver's NetGUID cache. With an external server or another replication backend the client view shows the replicated wait. Select the authoritative PIE world for full inspection.

The tab reads the world's in-memory trace. The timeline reports when older events have been discarded. Game modules can pass the executing component or actor as the optional `TraceTarget` argument to `FAngelIOOperation` to associate calls with the exact selected target, including after a rename. Debug data stays local to the editor process.

## Trace a ride on a listen server

1. Start the demo in PIE as a listen server with a connected player. Wait for the initial rides to finish. From the client's console, enter `LiftTrip 6 400`.
2. While the freight lift is moving, enter `LiftTrip 7 800` on the host. The host's request gets its own ID and completes with `Rejected` and reason `LiftBusy`. The client's original ride continues.
3. Enter `AngelIO.ExportTrace` in the host console for a snapshot, or stop PIE to export the final history. The Output Log prints the JSON file path under `Saved/AngelIO/Traces/`.
4. Call the MCP tool `read_trace` with `{}` to open the latest snapshot. Find a `call` for `Lift.MoveTo`, then pass its `operationId` and the returned `trace` filename to follow that ride. For large histories supply `limit` (1–200, default 50) and use `nextAfterSequence` as the next request's `afterSequence`.

| Event | What it records |
| --- | --- |
| `call` | Operation name, target, arguments, and caller. Script sites include the file, line, function and instance ID. Component-backed missions also identify their component, and player controllers identify local or remote ownership on the server. |
| `wait` | The script and line that read the handle. `suspended` is false when the result was already available. Repeated reads each have their own event. |
| `complete` | One terminal status per operation. A module can supply a reason such as `LiftBusy` or `TargetEndPlay`. |
| `resume` | A suspended script continues at its wait site after completion. |
| `detach` | The waiting script was released. The operation can still complete for other waiters. |
| `checkpointSaved` | The mission saved `stage` and `version`. The `caller` identifies the writing script instance and source line. |
| `checkpointRestored` | This script instance first read a matching checkpoint saved by an earlier instance. The `caller` identifies the reader and `savedByInstanceId` identifies the writer. |
| `stateSaved` | A native component committed a value. For a lift, `field` is `floor` and `value` is the floor reached. |
| `stateRestored` | The component applied a saved value. Its `stateId` matches the same record in the previous host's export. |

Each JSON snapshot includes a schema version, session ID, world and net mode. Events carry a sequence number and world time in seconds. Operation events carry an `operationId`. Checkpoint events share a `checkpointId` for the same room, component and script across reloads. A non-shipping authoritative game world retains its most recent 4096 events. `droppedEvents` counts older entries, so the beginning of a long ride can fall outside the captured window. PIE automatically exports worlds with recorded events. Operation history starts when a native operation is created. A request discarded by a controller before `MoveTo` has no trip ID.

## MCP tools for editors and agents

The local Model Context Protocol (MCP) server uses stdio. It requires Node.js, the project's built Unreal Editor target with AngelIO enabled, and the path to `UnrealEditor-Cmd.exe`. From the repository root, run `npm ci --prefix Tools/MCP`. Set the paths in your MCP client's configuration to match your checkout and engine installation.

```json
{
  "mcpServers": {
    "angelio": {
      "command": "node",
      "args": [
        "E:/DEV/AngelIO/Tools/MCP/server.mjs",
        "--project", "E:/DEV/AngelIO/AngelIO.uproject",
        "--editor", "E:/DEV/UEBASE/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
      ]
    }
  }
}
```

| Tool | Result |
| --- | --- |
| `list_scripts` | Paths relative to the project's `Content/AngelIO` directory |
| `read_script` | Source text of the selected mission and its SHA-256 revision for `write_script` |
| `describe_api` | Entry point, delivery rules, and the loaded game modules' function, method, and enum declarations in `externalDeclarations` |
| `validate_script` | Compile result, port types and directions, signal names, and compiler diagnostics with line and column |
| `validate_map` | Saved map package, mission count and diagnostics for components, inputs and named targets |
| `describe_scene` | Saved map actors and mission components by object path, including room-local name bindings and initial inputs |
| `write_script` | Compiles a proposed source, checks its SHA-256 revision and optionally commits the file. Defaults to a dry run |
| `read_trace` | Exported mission events, optionally filtered by operation, checkpoint or persisted state ID and paged by sequence number |

An agent can inspect the API, edit the source file in its working copy, and then call `validate_script`. Validation uses the plugin's headless commandlet. It compiles the code without executing global initializers or `Run`. Module-wide contract errors have line and column zero.

## Validation and tests

To generate a JSON report from PowerShell or CI, run the commandlet from the project root. Select a script with `-Script=Elevator.as` or a map with `-Map=/Game/Demo/TwoLifts`. The commandlet returns `0` on success, `1` for a validation error and `2` for invalid arguments or a report write failure.

```powershell
$editor = 'E:/DEV/UEBASE/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
& $editor "$PWD/AngelIO.uproject" -run=AngelIOCheck '-Script=Elevator.as' "-Report=$env:TEMP/angelio.json" -unattended -nullrhi -ddc=InstalledNoZenLocalFallback
```

Run the `AngelIO.` filter in Unreal Automation for the full regression suite. The `AngelIO.Network.ListenServer` filter selects the networked PIE tests.

| Test | What it checks |
| --- | --- |
| `NamedGroupAndLateJoin` | Starts a host and client using the IP NetDriver, calls the lift group, and connects a second client after the ride to check the replicated state. |
| `TypedTrips` | Sends overlapping host and client requests, starts a concurrent ride on a second lift, and verifies each caller's result and the exported event history, including cancellation when a moving lift is destroyed. |
| `HandleLifetimeAndTimeout` | Reassigns a name while a script holds the original lift. Checks a timed wait, eventual arrival, a client request to the replacement, replication and the stale handle's cancellation reason. |
| `StreamedTrips` | Loads the two-lift map as a sublevel on both peers, hides it mid-ride, shows the same actor again, then fully unloads and reloads the room. The client sees the new ride arrive. |
| `StreamedBytecode` | Verifies runtime hits and evictions in a streamed four-mission room with 0, 3 and 32 KiB budgets. [Results and reproduction](Docs/AgentGuide.md#runtime-bytecode-cache) |
| `WaitActor` | Sends client requests before loading and after unloading a room. Checks timeout, readiness after startup motion, replicated floor arrivals and lookup history on the host. |
| `MissionCheckpoints` | Interrupts and retries a delivery, reloads both committed stages, checks exactly one arrival and verifies the late client's state and a second room's independent progress. |
| `CheckpointRestart` | Restarts a host and client on the same slot. Both peers see floor 4 and the cab at its measured fourth-floor position. The mission accepts confirmation with zero new trips or delivery calls. |
| `MidRideRestart` | Stops the server while the host and client see the cab between floors 2 and 4. The new server starts the retry at floor 2. Both peers see one completed trip and the cab at floor 4. |
| `LandedBeforeDeliveredRestart` | Stops with floor 4 on disk and the mission still at `Dispatched`. Both peers recover floor 4 and wait for confirmation. The new host trace contains zero `Lift.MoveTo` calls and zero `Arrived` results. |
| `PartitionLanding` | Two players enter, leave and revisit a spatially streamed World Partition cell. Both see the saved fourth-floor landing on return. |
| `BlinkBranches` | The client sees the lamp pulse during a ride and stop on arrival. Unloading and reloading the room gives the panel a fresh branch. |
| `MissionInspector` | Opens the editor tab, selects a remote player's panel and a lift during a ride, checks source lines and the operation timeline, pins a result and verifies cleanup after destruction and PIE shutdown. |

To exercise the MCP tools with a real stdio client, use the same `$editor` path and run the smoke test after installing the Node dependencies. After running `TypedTrips`, add `--trace <export-filename.json>` to verify the host and remote histories through the MCP transport.

The release guide includes measured startup, memory and frame cost for 1–1000 waiting missions and commands to build and launch the Win64 Development and Shipping packages. [Release checks](Docs/AgentGuide.md#win64-release-and-scaling-checks)

For 1000 instances of the same mission, the guide also breaks down actor creation, API registration, compilation and VM startup, and measures the saving from loading bytecode. [Startup profile](Docs/AgentGuide.md#where-startup-time-goes)

The v0.6.3 editor experiment reduced mean startup time for those 1000 missions from 488.9 to 237.6 ms by registering the API once. Runtime mission components use world-owned registration by default from v0.7.0. [Shared registration results](Docs/AgentGuide.md#registering-the-api-once-for-1000-missions)

On the world engine, compiling `TimedGate.as` once and loading its bytecode for the other 999 actors cut mean startup from **248.1 to 190.8 ms (23.1%)** in the v0.7.1 editor experiment. The v0.7.0 tag measured 248.4 ms on the same machine. [Benchmark and isolation checks](Docs/AgentGuide.md#reusing-bytecode-on-the-world-engine)

```powershell
node Tools/MCP/smoke.mjs --project "$PWD/AngelIO.uproject" --editor $editor
```

[Architecture and development plan (in Russian)](Docs/Plan.md)

## License

AngelIO, the MCP server, and the demo project are available under the MIT license. AngelScript 2.39.0 and its `scriptstdstring` add-on use the zlib license. [MIT license](LICENSE) · [AngelScript license](Plugins/AngelIO/Source/ThirdParty/AngelScript/LICENSE)
