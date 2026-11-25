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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIORegistrationStartup, "AngelIO.Performance.SharedRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool CompareStartup(FAutomationTestBase& Test, bool bCompareRegistrations)
{
    auto RunBatch = [&](int32 Pair, bool bCandidate, int32 Count)
    {
        FTestWorldWrapper Scene;
        if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
        UWorld* World = Scene.GetTestWorld();
        World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
        FAngelIOStartupProbe Probe;
        Probe.bReuseBytecode = bCandidate && !bCompareRegistrations;
        Probe.bShareRegistrations = bCandidate && bCompareRegistrations;
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
                Test.AddError(FString::Printf(TEXT("Incomplete startup profile for mission %d"), I));
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
        if (!Test.TestTrue(TEXT("Every TimedGate is waiting with its initial output"), bWaiting) ||
            !Test.TestTrue(TEXT("First and last have distinct mission IDs"),
                First.Instance.IsValid() && Last.Instance.IsValid() && First.Instance != Last.Instance) ||
            !Test.TestEqual(TEXT("First mission script"), First.Script, FString(TEXT("Examples/TimedGate.as"))) ||
            !Test.TestEqual(TEXT("Last mission script"), Last.Script, First.Script) ||
            !Test.TestEqual(TEXT("First TimedGate wait line"), First.Line, 6) ||
            !Test.TestEqual(TEXT("Last TimedGate wait line"), Last.Line, 6) ||
            !Test.TestEqual(TEXT("API registrations"), Probe.Registrations, Probe.bShareRegistrations ? 1 : Count) ||
            !Test.TestEqual(TEXT("Source builds"), Probe.Builds, Probe.bReuseBytecode ? 1 : Count) ||
            !Test.TestEqual(TEXT("Bytecode loads"), Probe.Loads, Probe.bReuseBytecode ? Count - 1 : 0))
            return false;

        if (Pair > 0)
        {
            auto Ms = [](uint64 Cycles) { return FPlatformTime::ToMilliseconds64(Cycles); };
            const TCHAR* Mode = Probe.bShareRegistrations ? TEXT("shared_api") : Probe.bReuseBytecode ? TEXT("bytecode") : TEXT("source");
            Test.AddInfo(FString::Printf(TEXT("AngelIO startup pair=%d mode=%s count=%d registrations=%d builds=%d loads=%d bytes=%d ")
                TEXT("total_ms=%.3f spawn_ms=%.3f file_ms=%.3f engine_ms=%.3f core_ms=%.3f ")
                TEXT("providers_ms=%.3f sections_ms=%.3f module_ms=%.3f ")
                TEXT("inspect_ms=%.3f vm_start_ms=%.3f component_and_loop_ms=%.3f"),
                Pair, Mode, Count, Probe.Registrations, Probe.Builds, Probe.Loads,
                Probe.Bytecode.Num(), Ms(Total), Ms(Spawn), Ms(File), Ms(Engine), Ms(Core), Ms(Providers),
                Ms(Sections), Ms(Module), Ms(Inspect), Ms(Start), Ms(Other)));
        }
        return true;
    };

    // Warm both paths. Each measured cohort pays its own first engine registration and source build.
    if (!RunBatch(0, false, 8) || !RunBatch(0, true, 8)) return false;
    for (int32 Pair = 1; Pair <= 6; ++Pair)
    {
        const bool bCandidateFirst = Pair % 2 == 0;
        if (!RunBatch(Pair, bCandidateFirst, 1000) || !RunBatch(Pair, !bCandidateFirst, 1000)) return false;
    }
    return true;
}

bool FAngelIOMissionStartup::RunTest(const FString&) { return CompareStartup(*this, false); }
bool FAngelIORegistrationStartup::RunTest(const FString&) { return CompareStartup(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOSharedIsolation, "AngelIO.Missions.SharedRegistrationIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOSharedIsolation::RunTest(const FString&)
{
    constexpr int32 Count = 1000;
    FAngelIOStartupProbe Probe;
    Probe.bShareRegistrations = true;
    TArray<TUniquePtr<FAngelIOProgram>> Missions;
    for (int32 I = 0; I < Count; ++I)
    {
        auto Mission = MakeUnique<FAngelIOProgram>();
        FAngelIOValue Floor;
        Floor.Integer = I + 1;
        TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
        if (!Mission->CompileFile(TEXT("Elevator.as")) || !Mission->Start({{TEXT("StartingFloor"), Floor}}))
        {
            AddError(FString::Printf(TEXT("Could not start shared mission %d"), I));
            return false;
        }
        Missions.Add(MoveTemp(Mission));
    }
    if (!TestEqual(TEXT("All 1000 modules use one API registration"), Probe.Registrations, 1)) return false;
    {
        FAngelIOProgram Invalid;
        TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
        if (!TestFalse(TEXT("A sibling module can fail to build"), Invalid.Compile(TEXT("Invalid.as"), TEXT("void Run() {\nUnknown();\n}"))) ||
            !TestTrue(TEXT("The failing module owns its diagnostic"), Invalid.Diagnostics().ContainsByPredicate(
                [](const auto& D) { return D.Severity == TEXT("error") && D.Line == 2; })) ||
            !TestTrue(TEXT("A valid sibling retains clean diagnostics"), Missions.Last()->Diagnostics().IsEmpty())) return false;
    }
    const TWeakPtr<FAngelIOBenchmarkEngine> Lifetime = Probe.SharedEngine;
    Probe.SharedEngine.Reset();
    for (int32 I = 0; I < Count; ++I)
    {
        // Leave one group idle, one in motion and one back at the first wait after a completed trip.
        if (I % 3 != 0 && !Missions[I]->Signal(TEXT("RequestFloor"), Count + I)) return false;
        if (I % 3 == 2 && !Missions[I]->Signal(TEXT("MotionFinished"), Count + I)) return false;
    }
    TSet<FGuid> Instances;
    for (int32 I = 0; I < Count; ++I)
    {
        const auto& Mission = *Missions[I];
        const auto Site = Mission.InspectMission();
        FAngelIOValue Floor, Trips;
        const bool bMoving = I % 3 == 1, bArrived = I % 3 == 2;
        if (!Mission.GetOutput(TEXT("LastFloor"), Floor) || Floor.Integer != (bArrived ? Count + I : I + 1) ||
            !Mission.GetOutput(TEXT("TotalTrips"), Trips) || Trips.Integer != (bArrived ? 1 : 0) ||
            Site.State != EAngelIOState::Waiting || Site.Waiting != (bMoving ? TEXT("MotionFinished") : TEXT("RequestFloor")) ||
            Site.Script != TEXT("Elevator.as") || Site.Line != (bMoving ? 25 : 18) || !Site.Instance.IsValid())
        {
            AddError(FString::Printf(TEXT("Mission %d has incorrect globals, wait or source location"), I));
            return false;
        }
        Instances.Add(Site.Instance);
    }
    Missions.Reset();
    return TestEqual(TEXT("Distinct mission IDs"), Instances.Num(), Count) &&
        TestFalse(TEXT("Releasing the final mission releases its registered engine"), Lifetime.IsValid());
}
#endif
