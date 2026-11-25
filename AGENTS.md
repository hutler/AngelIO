# Working on AngelIO

AngelIO is a Win64 Unreal Engine plugin for writing level missions in AngelScript. The repository includes the runtime, editor tools, a playable two-lift example, and a local MCP server. Start with the [agent guide](Docs/AgentGuide.md) for build commands, mission authoring, native operations, multiplayer debugging, and test recipes. The [README](README.md) covers running the demo and installing the plugin in a game.

## Pick the right entry point

| When the task concerns | Start here |
| --- | --- |
| A mission, an input, or an output | [Mission files](Content/AngelIO) and the [component contract](Plugins/AngelIO/Source/AngelIO/Public/AngelIOScriptComponent.h) |
| Finding an actor or sending to a named group | [World registry](Plugins/AngelIO/Source/AngelIO/Private/AngelIOTargetRegistry.cpp) |
| An external module's typed API | [Provider interface](Plugins/AngelIO/Source/AngelIO/Public/AngelIOScriptAPI.h) and [demo provider](Source/AngelIOHost/AngelIOHost.cpp) |
| A network request or an unexpected wait | [Player controller](Source/AngelIOHost/AngelIOPlayerController.cpp), [PIE inspector](Plugins/AngelIO/Source/AngelIOEditor/Private/SAngelIOMissionInspector.cpp), and [listen-server tests](Source/AngelIOHost/Tests/AngelIOListenServerTests.cpp) |
| What an agent can discover and validate | [MCP tool definitions](Tools/MCP/server.mjs) and [stdio smoke test](Tools/MCP/smoke.mjs) |

## Project workflow

- Bump AngelIO's version in every submission, in the same commit as the change. Increment the integer `Version` in `AngelIO.uplugin` and keep `VersionName`, the MCP package and its lockfile in sync. Use a patch bump for fixes and a minor bump for new features.
- On a fresh clone with no local Beads database, run `bd bootstrap --yes` from the repository root, then `bd ready`. Bootstrap reads the project's sync configuration and sets up the local Dolt store. If `bd ready` already works, continue with that workspace.
- Track work in Beads. Run `bd prime` to recover context, `bd ready` to find available work, and `bd show <id>` before claiming it with `bd update <id> --claim`. For a new request, create an issue with `bd create`. Record enduring project knowledge with `bd remember`. Close completed work with `bd close <id> --reason "..."`.
- Keep game-specific types and rules in the game module. The plugin owns mission execution, addressing, replication of public state, and generic operation tracing. The editor module owns the inspector and validation commandlet.
- Discover the loaded bindings with `describe_api` when writing a mission. Edit the UTF-8 `.as` file in the checkout and compile it with `validate_script` or the commandlet. Use PIE to observe runtime behavior.
- For multiplayer work, exercise a listen server and a connected client. This project's network test scope is listen-server PIE. Prefer the existing `AngelIO.Network.ListenServer` scenarios when changing authority, replication, operation lifetime, or the inspector.
- Report the commands and outcomes you verified. For a disputed ride, record the PIE world, script and line, operation ID, and terminal result when available. Keep generated builds, local Unreal settings and trace exports out of source commits.
