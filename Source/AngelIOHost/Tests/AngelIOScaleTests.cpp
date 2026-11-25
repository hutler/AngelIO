#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOScriptComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AngelIOStartupProbe.h"

namespace
{
    UAngelIOScriptComponent* CreateTimedGate(AActor* Actor)
    {
        auto* Mission = NewObject<UAngelIOScriptComponent>(Actor);
        Actor->AddInstanceComponent(Mission);
        Mission->Script = TEXT("Examples/TimedGate.as");
        FAngelIOValue Delay;
        Delay.Type = EAngelIOType::Float;
        Delay.Float = 60.f;
        Mission->InitialInputs.Add(TEXT("Delay"), Delay);
        return Mission;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOScale, "AngelIO.Performance.WaitingMissions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOScale::RunTest(const FString&)
{
    for (const int32 Count : {1, 100, 1000})
    {
        FTestWorldWrapper Scene;
        if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
        UWorld* World = Scene.GetTestWorld();
        World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
        auto FrameMs = [&Scene]
        {
            constexpr int32 Frames = 120;
            const double Start = FPlatformTime::Seconds();
            for (int32 I = 0; I < Frames; ++I) Scene.TickTestWorld(1.f / 60.f);
            return (FPlatformTime::Seconds() - Start) * 1000 / Frames;
        };
        const double Baseline = FrameMs();
        const int64 Before = FPlatformMemory::GetStats().UsedPhysical;
        const double Start = FPlatformTime::Seconds();
        TArray<UAngelIOScriptComponent*> Missions;
        for (int32 I = 0; I < Count; ++I)
        {
            auto* Actor = World->SpawnActor<AActor>();
            auto* Mission = CreateTimedGate(Actor);
            Mission->RegisterComponent();
            Missions.Add(Mission);
        }
        const double StartMs = (FPlatformTime::Seconds() - Start) * 1000;
        const int64 Bytes = int64(FPlatformMemory::GetStats().UsedPhysical) - Before;
        const double WaitingMs = FrameMs();
        if (!TestTrue(TEXT("Every independent VM is still waiting"), !Missions.ContainsByPredicate([](const auto* M)
            { return M->GetWaitingSignal() != TEXT("WaitSeconds") || !M->IsComponentTickEnabled(); }))) return false;
        AddInfo(FString::Printf(TEXT("AngelIO benchmark count=%d startup_ms=%.3f resident_delta_mib=%.2f baseline_frame_ms=%.4f waiting_frame_ms=%.4f"),
            Count, StartMs, double(Bytes) / (1024.0 * 1024.0), Baseline, WaitingMs));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOMissionStartup, "AngelIO.Performance.MissionStartup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOMissionStartup::RunTest(const FString&)
{
    auto RunBatch = [this](int32 Pair, bool bReuseBytecode, int32 Count)
    {
        FTestWorldWrapper Scene;
        if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
        UWorld* World = Scene.GetTestWorld();
        World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
        FAngelIOStartupProbe Probe;
        Probe.bReuseBytecode = bReuseBytecode;
        TArray<UAngelIOScriptComponent*> Missions;
        Missions.Reserve(Count);
        uint64 Spawn = 0, File = 0, Engine = 0, Core = 0, Providers = 0, Sections = 0;
        uint64 Module = 0, Inspect = 0, Start = 0;
        const uint64 BatchBegin = FPlatformTime::Cycles64();
        for (int32 I = 0; I < Count; ++I)
        {
            const uint64 ActorBegin = FPlatformTime::Cycles64();
            auto* Actor = World->SpawnActor<AActor>();
            const uint64 ActorEnd = FPlatformTime::Cycles64();
            if (!Actor) return false;
            auto* Mission = CreateTimedGate(Actor);
            {
                TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
                Mission->RegisterComponent();
            }
            // A compile or startup failure can leave a timestamp from the preceding instance.
            if (Probe.FileBegin < ActorEnd || Probe.CompileBegin < Probe.FileBegin ||
                Probe.EngineCreated < Probe.CompileBegin || Probe.CoreRegistered < Probe.EngineCreated ||
                Probe.ProvidersInspected < Probe.CoreRegistered || Probe.ModuleBegin < Probe.ProvidersInspected ||
                Probe.ModuleEnd < Probe.ModuleBegin ||
                Probe.CompileEnd < Probe.ModuleEnd || Probe.StartEnd < Probe.CompileEnd)
            {
                AddError(FString::Printf(TEXT("Incomplete startup profile for mission %d"), I));
                return false;
            }
            Spawn += ActorEnd - ActorBegin;
            File += Probe.CompileBegin - Probe.FileBegin;
            Engine += Probe.EngineCreated - Probe.CompileBegin;
            Core += Probe.CoreRegistered - Probe.EngineCreated;
            Providers += Probe.ProvidersInspected - Probe.CoreRegistered;
            Sections += Probe.ModuleBegin - Probe.ProvidersInspected;
            Module += Probe.ModuleEnd - Probe.ModuleBegin;
            Inspect += Probe.CompileEnd - Probe.ModuleEnd;
            Start += Probe.StartEnd - Probe.CompileEnd;
            Missions.Add(Mission);
        }
        const uint64 Total = FPlatformTime::Cycles64() - BatchBegin;
        const uint64 Other = Total - Spawn - File - Engine - Core - Providers - Sections - Module - Inspect - Start;

        const bool bWaiting = !Missions.ContainsByPredicate([](const auto* Mission)
        {
            FAngelIOValue Open;
            return !Mission->IsComponentTickEnabled() || Mission->GetWaitingSignal() != TEXT("WaitSeconds") ||
                !Mission->GetOutput(TEXT("Open"), Open) || Open.Type != EAngelIOType::Boolean || Open.Boolean;
        });
        const auto First = Missions[0]->InspectMission();
        const auto Last = Missions.Last()->InspectMission();
        if (!TestTrue(TEXT("Every TimedGate is waiting with its initial output"), bWaiting) ||
            !TestTrue(TEXT("First and last have distinct VMs and the same mission source line"),
                First.Instance.IsValid() && Last.Instance.IsValid() && First.Instance != Last.Instance &&
                First.Line > 0 && First.Line == Last.Line && First.Script == Last.Script) ||
            !TestEqual(TEXT("Source builds"), Probe.Builds, bReuseBytecode ? 1 : Count) ||
            !TestEqual(TEXT("Bytecode loads"), Probe.Loads, bReuseBytecode ? Count - 1 : 0))
            return false;

        if (Pair > 0)
        {
            auto Ms = [](uint64 Cycles) { return FPlatformTime::ToMilliseconds64(Cycles); };
            AddInfo(FString::Printf(TEXT("AngelIO startup pair=%d mode=%s count=%d builds=%d loads=%d bytes=%d ")
                TEXT("total_ms=%.3f spawn_ms=%.3f file_ms=%.3f engine_ms=%.3f core_ms=%.3f ")
                TEXT("providers_ms=%.3f sections_ms=%.3f module_ms=%.3f ")
                TEXT("inspect_ms=%.3f vm_start_ms=%.3f component_and_loop_ms=%.3f"),
                Pair, bReuseBytecode ? TEXT("bytecode") : TEXT("source"), Count, Probe.Builds, Probe.Loads,
                Probe.Bytecode.Num(), Ms(Total), Ms(Spawn), Ms(File), Ms(Engine), Ms(Core), Ms(Providers),
                Ms(Sections), Ms(Module), Ms(Inspect), Ms(Start), Ms(Other)));
        }
        return true;
    };

    // Warm the file and both SDK paths. Each measured bytecode cohort still pays its own first build and save.
    if (!RunBatch(0, false, 8) || !RunBatch(0, true, 8)) return false;
    for (int32 Pair = 1; Pair <= 6; ++Pair)
    {
        const bool bBytecodeFirst = Pair % 2 == 0;
        if (!RunBatch(Pair, bBytecodeFirst, 1000) || !RunBatch(Pair, !bBytecodeFirst, 1000)) return false;
    }
    return true;
}
#endif
