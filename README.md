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

Open [AngelIO.uproject](AngelIO.uproject) with Unreal Engine 5.8, build the C++ modules if prompted, and press **Play** on the default map, `/Game/Demo/TwoLifts`. Each platform runs its own instance of `Elevator.as`. The lifts start on floor 1 and travel to floors 2 and 3. Each floor is 200 cm apart and the default speed is 240 cm/s.

Once the platforms stop, try either command in the player console. Both work from the listen-server host and from a connected player.

| Command | What to expect |
| --- | --- |
| `LiftTrip 6 400` | The freight lift moves to floor 6 at 400 cm/s. The requesting player's `TripPanel` receives the result for that ride. |
| `LiftFloor 4` | The panel sends `RequestFloor` to the `DemoLifts` group. Both idle lifts move to floor 4. |

To change the initial ride, select a placed `AngelIOExampleLift` and edit **Requested Floor**. The `Mission` component's **Initial Inputs** accepts `StartingFloor` (int), `TravelSpeed` (float, in cm/s), and `Enabled` (bool). A Blueprint can request another ride with **Request Floor**.

## Call a typed API and wait for a trip

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
| `TripStatus::Cancelled` | The lift was destroyed or its mission stopped during the ride. |

`trip.Id` and `result.Id` return the same operation ID as a string. Use it to look up the ride in an exported trace.

The playable version takes the floor and speed from the player's command. It publishes `LastStatus`, `LastFloor`, and `Completed` on `TripPanel` for the owning player. [Demo mission](Content/AngelIO/TypedLift.as)

## Address actors by name

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

## Use the plugin in your project

Copy [Plugins/AngelIO](Plugins/AngelIO) into a C++ project and enable the plugin. Put mission files in `Content/AngelIO`. Add `AngelIO` to **Project Settings → Packaging → Additional Non-Asset Directories to Package** so newly added scripts ship with the game. The demo already includes this setting. [Demo packaging configuration](Config/DefaultGame.ini)

Add `AngelIO` to the dependencies in your module's `Build.cs` to use the component or register script bindings. The plugin ships with the AngelScript 2.39.0 static library for Win64. The rebuild script verifies the SDK archive's SHA-256 and uses MSVC 2022 x64. Other platforms require a library build for that target. [Rebuild script](Tools/BuildAngelScript.ps1) · [AngelScript SDK](https://www.angelcode.com/angelscript/downloads.html)

### Expose types from a game module

Implement `IAngelIOScriptAPI::RegisterBindings(asIScriptEngine&)`. Register the provider with `IModularFeatures` during `StartupModule` and unregister it during `ShutdownModule`. AngelIO calls the provider for each new VM, including the validation commandlet's VM. Keep the module loaded for the lifetime of its VMs. [Provider interface](Plugins/AngelIO/Source/AngelIO/Public/AngelIOScriptAPI.h) · [Demo bindings](Source/AngelIOHost/AngelIOHost.cpp)

Inside a native binding, `AngelIO::GetScriptWorld()` gives access to the current mission's world. For a long-running action, store the result in a type derived from `FAngelIOOperation`. Write the result and call `MarkComplete()` on the game thread. Pass the same operation to `AngelIO::AwaitOperation` when the script waits. The component checks completion on its tick while that wait is active. Stopping the caller releases its wait. The lift continues to own any ride it has already started. [Lift implementation](Source/AngelIOHost/AngelIOExampleLift.cpp)

Every `FAngelIOOperation` gets a unique `FGuid`, available through `GetOperationId()`. To describe it in the trace, pass the world, operation name, logical target, and optional JSON arguments to the base constructor. A binding running inside a mission can use its current script world. A native producer outside a script should pass its world explicitly. Call `MarkComplete(Status, Reason)` after recording the domain result. AngelIO captures the active caller and each waiter separately, including when they belong to different script instances. [Lift operation](Source/AngelIOHost/AngelIOLiftTrip.cpp)

### Multiplayer

The server runs the missions and resolves named targets. Enable **Replicates** on the owning actor to publish component state. On a client, `GetOutput` and `GetWaitingSignal` read the latest replicated values. Subscribe to `OnPublicStateChanged` to update UI, including for late joiners. Inputs and the execution stack stay on the server. Intermediate changes can coalesce between network updates.

`OnSignal` drives authoritative gameplay. For a sound or visual effect, add the output event name to **Presentation Signals** and subscribe to `OnPresentationSignal`. The host gets one local notification. Relevant clients receive an unreliable multicast carrying its own payload. An effect may arrive before the output update or be lost. A late joiner receives the current outputs without replaying earlier effects.

In the demo, the player's controller sends the request to the server. `LiftFloor 4` and the Blueprint call `ServerRequestLiftFloor(4)` both reach the group through `LiftPanel.as`. The typed `LiftTrip` console command accepts floors 1–8 and speeds up to 2000 cm/s. Each game's module controls which commands and arguments players may send. [Demo controller](Source/AngelIOHost/AngelIOPlayerController.cpp)

## Inspect a mission during PIE

Open **Tools → AngelIO Mission Inspector** in the Unreal Editor. Start the demo with **Play As Listen Server**, two players and **Run Under One Process**. Wait for the initial rides to finish, then enter `LiftTrip 6 100` in the client's console.

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

Each JSON snapshot includes a schema version, session ID, world and net mode. Events carry a sequence number, world time in seconds and operation ID. A non-shipping authoritative game world retains its most recent 4096 events. `droppedEvents` counts older entries, so the beginning of a long ride can fall outside the captured window. PIE automatically exports worlds with recorded events. Operation history starts when a native operation is created. A request discarded by a controller before `MoveTo` has no trip ID.

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
| `read_script` | Source text of the selected mission |
| `describe_api` | Entry point, delivery rules, and the loaded game modules' function, method, and enum declarations in `externalDeclarations` |
| `validate_script` | Compile result, port types and directions, signal names, and compiler diagnostics with line and column |
| `read_trace` | An exported operation history, optionally filtered by operation ID and paged by sequence number |

An agent can inspect the API, edit the source file in its working copy, and then call `validate_script`. Validation uses the plugin's headless commandlet. It compiles the code without executing global initializers or `Run`. Module-wide contract errors have line and column zero.

## Validation and tests

To generate the same JSON report from PowerShell or CI, run the commandlet from the project root. It returns `0` on success, `1` for an invalid script, or `2` for invalid arguments or a report write failure.

```powershell
$editor = 'E:/DEV/UEBASE/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
& $editor "$PWD/AngelIO.uproject" -run=AngelIOCheck '-Script=Elevator.as' "-Report=$env:TEMP/angelio.json" -unattended -nullrhi -ddc=InstalledNoZenLocalFallback
```

Run the `AngelIO.` filter in Unreal Automation for the full regression suite. The `AngelIO.Network.ListenServer` filter selects the networked PIE tests.

| Test | What it checks |
| --- | --- |
| `NamedGroupAndLateJoin` | Starts a host and client using the IP NetDriver, calls the lift group, and connects a second client after the ride to check the replicated state. |
| `TypedTrips` | Sends overlapping host and client requests, starts a concurrent ride on a second lift, and verifies each caller's result and the exported event history, including cancellation when a moving lift is destroyed. |
| `MissionInspector` | Opens the editor tab, selects a remote player's panel and a lift during a ride, checks source lines and the operation timeline, pins a result and verifies cleanup after destruction and PIE shutdown. |

To exercise the MCP tools with a real stdio client, use the same `$editor` path and run the smoke test after installing the Node dependencies. After running `TypedTrips`, add `--trace <export-filename.json>` to verify the host and remote histories through the MCP transport.

```powershell
node Tools/MCP/smoke.mjs --project "$PWD/AngelIO.uproject" --editor $editor
```

[Architecture and development plan (in Russian)](Docs/Plan.md)

## License

AngelIO, the MCP server, and the demo project are available under the MIT license. AngelScript 2.39.0 and its `scriptstdstring` add-on use the zlib license. [MIT license](LICENSE) · [AngelScript license](Plugins/AngelIO/Source/ThirdParty/AngelScript/LICENSE)
