#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOScriptComponent.h"
#include "AngelIOScriptAPI.h"
#include "AngelIOBytecodeCacheStats.h"
#include "AngelIOSettings.h"
#include "Features/IModularFeatures.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOWorldBytecodeStartup, "AngelIO.Performance.WorldBytecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

enum class EStartupMode { PrivateSource, PrivateBytecode, WorldSource, WorldBytecode };

static bool CompareStartup(FAutomationTestBase& Test, EStartupMode Baseline, EStartupMode Candidate)
{
    auto RunBatch = [&](int32 Pair, EStartupMode Mode, int32 Count)
    {
        FTestWorldWrapper Scene;
        if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
        UWorld* World = Scene.GetTestWorld();
        World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
        const bool bReuseBytecode = Mode == EStartupMode::PrivateBytecode || Mode == EStartupMode::WorldBytecode;
        FAngelIOStartupProbe Probe(bReuseBytecode ? 1 : 0);
        Probe.bForcePrivateEngine = Mode == EStartupMode::PrivateSource || Mode == EStartupMode::PrivateBytecode;
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
            !Test.TestEqual(TEXT("API registrations"), Probe.Registrations, Probe.bForcePrivateEngine ? Count : 1) ||
            !Test.TestEqual(TEXT("Source builds"), Probe.Builds, bReuseBytecode ? 1 : Count) ||
            !Test.TestEqual(TEXT("Bytecode loads"), Probe.Loads, bReuseBytecode ? Count - 1 : 0))
            return false;

        if (Pair > 0)
        {
            auto Ms = [](uint64 Cycles) { return FPlatformTime::ToMilliseconds64(Cycles); };
            const TCHAR* Label = TEXT("source");
            switch (Mode)
            {
            case EStartupMode::PrivateSource: break;
            case EStartupMode::PrivateBytecode: Label = TEXT("bytecode"); break;
            case EStartupMode::WorldSource: Label = TEXT("shared_api"); break;
            case EStartupMode::WorldBytecode: Label = TEXT("world_bytecode"); break;
            }
            Test.AddInfo(FString::Printf(TEXT("AngelIO startup pair=%d mode=%s count=%d registrations=%d builds=%d loads=%d bytes=%lld ")
                TEXT("total_ms=%.3f spawn_ms=%.3f file_ms=%.3f engine_ms=%.3f core_ms=%.3f ")
                TEXT("providers_ms=%.3f sections_ms=%.3f module_ms=%.3f ")
                TEXT("inspect_ms=%.3f vm_start_ms=%.3f component_and_loop_ms=%.3f"),
                Pair, Label, Count, Probe.Registrations, Probe.Builds, Probe.Loads,
                Probe.PayloadBytes(), Ms(Total), Ms(Spawn), Ms(File), Ms(Engine), Ms(Core), Ms(Providers),
                Ms(Sections), Ms(Module), Ms(Inspect), Ms(Start), Ms(Other)));
        }
        return true;
    };

    // Warm both paths. Each measured cohort pays its own first engine registration and source build.
    if (!RunBatch(0, Baseline, 8) || !RunBatch(0, Candidate, 8)) return false;
    for (int32 Pair = 1; Pair <= 6; ++Pair)
    {
        const bool bCandidateFirst = Pair % 2 == 0;
        if (!RunBatch(Pair, bCandidateFirst ? Candidate : Baseline, 1000) ||
            !RunBatch(Pair, bCandidateFirst ? Baseline : Candidate, 1000)) return false;
    }
    return true;
}

bool FAngelIOMissionStartup::RunTest(const FString&)
{
    return CompareStartup(*this, EStartupMode::PrivateSource, EStartupMode::PrivateBytecode);
}

bool FAngelIORegistrationStartup::RunTest(const FString&)
{
    return CompareStartup(*this, EStartupMode::PrivateSource, EStartupMode::WorldSource);
}

bool FAngelIOWorldBytecodeStartup::RunTest(const FString&)
{
    return CompareStartup(*this, EStartupMode::WorldSource, EStartupMode::WorldBytecode);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOWorldOwnership, "AngelIO.Missions.WorldEngineOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOWorldOwnership::RunTest(const FString&)
{
    struct FProvider final : IAngelIOScriptAPI
    {
        int32 Attempts = 0;
        FProvider() { IModularFeatures::Get().RegisterModularFeature(FeatureName(), this); }
        ~FProvider() { IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this); }
        bool RegisterBindings(asIScriptEngine&) override { return ++Attempts > 1; }
    } Provider;
    FTestWorldWrapper First, Second;
    if (!First.CreateTestWorld(EWorldType::Game) || !Second.CreateTestWorld(EWorldType::Game)) return false;
    UWorld* A = First.GetTestWorld();
    UWorld* B = Second.GetTestWorld();
    const FString Label = TEXT("World.as"), Source = TEXT("int out_Value = 3; void Run() { out_Value++; }");
    {
        FAngelIOProgram Retry;
        if (!TestFalse(TEXT("The provider rejected its first registration"), Retry.Compile(Label, Source, A)) ||
            !TestFalse(TEXT("Failed registration was never published"), FAngelIOStartupProbe::WorldEngine(A).IsValid()) ||
            !TestTrue(TEXT("Retry builds with a fresh engine"), Retry.Compile(Label, Source, A)) ||
            !TestFalse(TEXT("A mission cannot start in a different world"), Retry.Start({}, B))) return false;
    }
    const auto Original = FAngelIOStartupProbe::WorldEngine(A);
    if (!TestTrue(TEXT("An empty world retains its registered API"), Original.IsValid()) ||
        !TestEqual(TEXT("Stopped mission discarded its module"), FAngelIOStartupProbe::WorldModuleCount(A), 0)) return false;

    FAngelIOProgram One, Two;
    if (!One.Compile(Label, Source, A) || !Two.Compile(Label, Source, B) ||
        !TestTrue(TEXT("Concurrent worlds own distinct engines"), Original.Pin() != FAngelIOStartupProbe::WorldEngine(B).Pin()) ||
        !TestEqual(TEXT("One failed attempt plus one successful registration per world"), Provider.Attempts, 3)) return false;
    if (!First.DestroyTestWorld(true)) return false;
    FAngelIOValue Output;
    return TestFalse(TEXT("First world released its engine"), Original.IsValid()) &&
        TestFalse(TEXT("Its precompiled mission cannot start after teardown"), One.Start({})) &&
        TestTrue(TEXT("Second world can still execute its globals"), Two.Start({}) &&
            Two.GetOutput(TEXT("Value"), Output) && Output.Integer == 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOSharedIsolation, "AngelIO.Missions.SharedRegistrationIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIORuntimeBytecode, "AngelIO.Missions.RuntimeBytecodeBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIORuntimeBytecode::RunTest(const FString&)
{
    FTestWorldWrapper Reference, Limited;
    if (!Reference.CreateTestWorld(EWorldType::Game) || !Limited.CreateTestWorld(EWorldType::Game)) return false;
    UWorld* A = Reference.GetTestWorld();
    UWorld* B = Limited.GetTestWorld();
    auto Source = [](int32 Length)
    {
        return FString::Printf(TEXT("int out_Value; void Run() { string s = \"%s\"; out_Value = int(s.length()); }"),
            *FString::ChrN(Length, TEXT('x')));
    };
    const FString Body = Source(2048);
    auto Run = [this](UWorld* World, const TCHAR* Label, const FString& Code, int32 Expected)
    {
        FAngelIOProgram Mission;
        FAngelIOValue Value;
        return TestTrue(TEXT("Requested source revision executes"), Mission.Compile(Label, Code, World) &&
            Mission.Start({}) && Mission.GetOutput(TEXT("Value"), Value) && Value.Integer == Expected);
    };
    TGuardValue<int32> ProjectBudget(GetMutableDefault<UAngelIOSettings>()->BytecodeCacheBudgetKiB, 64);
    if (!Run(A, TEXT("A.as"), Body, 2048)) return false;
    const auto Ref = AngelIO::GetBytecodeCacheStats(A);
    if (!TestTrue(TEXT("Reference world uses the project budget"), Ref && Ref->BudgetBytes == 64 * 1024 &&
        Ref->Entries == 1 && Ref->RetainedBytes > 1024)) return false;
    // Select a budget that holds two equal-size entries, including allocator rounding.
    const int32 KiB = int32((2 * Ref->RetainedBytes + Ref->RetainedBytes / 2 + 512) / 1024);
    B->URL.AddOption(*FString::Printf(TEXT("AngelIOBytecodeCacheKiB=%d"), KiB));
    auto Counts = [&](uint64 Hits, uint64 Builds, uint64 Evictions)
    {
        const auto Stats = AngelIO::GetBytecodeCacheStats(B);
        return TestTrue(TEXT("Expected runtime hit/build/eviction counts within the byte budget"), Stats &&
            Stats->Hits == Hits && Stats->SourceBuilds == Builds && Stats->Evictions == Evictions &&
            Stats->RetainedBytes <= Stats->BudgetBytes && Stats->BudgetBytes == uint64(KiB) * 1024);
    };
    if (!Run(B, TEXT("A.as"), Body, 2048) || !Run(B, TEXT("B.as"), Body, 2048) ||
        !Run(B, TEXT("A.as"), Body, 2048) || !Run(B, TEXT("C.as"), Body, 2048) || !Counts(1, 3, 1) ||
        !Run(B, TEXT("A.as"), Body, 2048) || !Run(B, TEXT("B.as"), Body, 2048) || !Counts(2, 4, 2)) return false;
    const auto Resident = *AngelIO::GetBytecodeCacheStats(B);
    const int32 Large = KiB * 2048;
    if (!Run(B, TEXT("Large.as"), Source(Large), Large) || !Counts(2, 5, 2)) return false;
    const auto After = *AngelIO::GetBytecodeCacheStats(B);
    if (!TestTrue(TEXT("Oversized mission preserved the resident entries"),
        After.Oversized == 1 && After.RetainedBytes == Resident.RetainedBytes && After.Entries == 2)) return false;
    const FString Edit = Source(2047);
    if (!Run(B, TEXT("A.as"), Edit, 2047) || !Run(B, TEXT("A.as"), Edit, 2047) || !Counts(3, 6, 2)) return false;
    FAngelIOProgram Broken;
    if (!TestFalse(TEXT("Bad edit receives its own compiler failure"), Broken.Compile(TEXT("A.as"), TEXT("void Run("), B)) ||
        !Run(B, TEXT("A.as"), Edit, 2047) || !Counts(4, 7, 2)) return false;
    const auto Detail = AngelIO::GetBytecodeCacheSnapshot(B);
    if (!TestTrue(TEXT("History records only hits and budget evictions"), Detail && Detail->RecentActivity.Num() == 6)) return false;
    const FString ExpectedLabels[] = {TEXT("A.as"), TEXT("B.as"), TEXT("A.as"), TEXT("C.as"), TEXT("A.as"), TEXT("A.as")};
    for (int32 I = 0; I < 6; ++I)
    {
        const auto& Event = Detail->RecentActivity[I];
        TestEqual(TEXT("Budget eviction names the victim, including after readmission"), Event.Entry.Script, ExpectedLabels[I]);
        TestEqual(TEXT("Source replacement and oversized builds add no eviction"), Event.Action == EAngelIOBytecodeCacheAction::Eviction, I == 1 || I == 3);
    }
    return TestTrue(TEXT("Concurrent world's cache stayed independent"),
        AngelIO::GetBytecodeCacheStats(A)->SourceBuilds == 1 && AngelIO::GetBytecodeCacheStats(A)->Hits == 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCacheActivityRetention, "AngelIO.Missions.BytecodeCacheActivityRetention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOCacheActivityRetention::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    UWorld* World = Scene.GetTestWorld();
    World->URL.AddOption(TEXT("AngelIOBytecodeCacheKiB=32"));
    const TCHAR* Labels[] = {TEXT("Panel.as"), TEXT("panel.as")};
    for (int32 I = 0; I < 142; ++I)
    {
        FAngelIOProgram Mission;
        if (!TestTrue(TEXT("Compile both case-sensitive labels"), Mission.Compile(Labels[I % 2], TEXT("void Run() {}"), World))) return false;
    }
    const auto Cache = AngelIO::GetBytecodeCacheSnapshot(World);
    if (!TestTrue(TEXT("Bounded journal retains the newest 128 of 140 hits"),
        Cache && Cache->Hits == 140 && Cache->RecentActivity.Num() == 128 && Cache->DroppedActivity == 12 &&
        Cache->ResidentScripts.Num() == 2)) return false;
    TestTrue(TEXT("Most recently used label keeps its case"), Cache->ResidentScripts[0].Script.Equals(Labels[1], ESearchCase::CaseSensitive));
    for (int32 I = 0; I < Cache->RecentActivity.Num(); ++I)
    {
        const auto& Event = Cache->RecentActivity[I];
        TestEqual(TEXT("Cache sequence survives ring wrap"), Event.Sequence, uint64(13 + I));
        TestTrue(TEXT("Hit keeps the exact script and allocation charge"), Event.Action == EAngelIOBytecodeCacheAction::Hit &&
            Event.Entry.Script.Equals(Labels[I % 2], ESearchCase::CaseSensitive) &&
            Event.Entry.RetainedBytes == Cache->ResidentScripts[1 - I % 2].RetainedBytes);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOBytecodeIsolation, "AngelIO.Missions.WorldBytecodeIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCacheRevisions, "AngelIO.Missions.BytecodeCacheRevisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOCacheRevisions::RunTest(const FString&)
{
    // Worldless validation collects the provider's current wrapper body on every compile.
    struct FProvider final : IAngelIOScriptAPI
    {
        FProvider() { IModularFeatures::Get().RegisterModularFeature(FeatureName(), this); }
        ~FProvider() { IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this); }
        bool RegisterBindings(asIScriptEngine&) override { return true; }
        void GetScriptWrappers(TArray<FAngelIOScriptWrapper>& Out) const override
        {
            Out.Add({TEXT("int CacheValue()"), FString::Printf(TEXT("return %d;"), Value)});
        }
        int32 Value = 2;
    } Provider;
    FAngelIOStartupProbe Probe(3);
    TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
    auto Run = [this, &Probe](const FString& Source, int32 Expected, int32 Builds, int32 Hits)
    {
        FAngelIOProgram Mission;
        FAngelIOValue Output;
        return TestTrue(TEXT("Execute the requested revision"), Mission.Compile(TEXT("Revision.as"), Source) &&
            Mission.Start({}) && Mission.GetOutput(TEXT("Value"), Output) && Output.Integer == Expected) &&
            TestEqual(TEXT("Source builds"), Probe.Builds, Builds) && TestEqual(TEXT("Cache hits"), Probe.Loads, Hits) &&
            TestEqual(TEXT("One retained revision per label"), Probe.CacheEntries(), 1);
    };
    const FString First = TEXT("int out_Value; void Run() { out_Value = CacheValue(); }");
    const FString Edited = TEXT("int out_Value; void Run() { out_Value = 1 + CacheValue(); }");
    if (!Run(First, 2, 1, 0) || !Run(First, 2, 1, 1) || !Run(Edited, 3, 2, 1)) return false;
    Provider.Value = 7;
    if (!Run(Edited, 8, 3, 1)) return false;
    FAngelIOProgram Invalid;
    return TestFalse(TEXT("Broken edit fails to compile"), Invalid.Compile(TEXT("Revision.as"), TEXT("void Run("))) &&
        Run(Edited, 8, 4, 2);
}

static bool CheckIsolation(FAutomationTestBase& Test, bool bReuseBytecode)
{
    constexpr int32 Count = 1000;
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    UWorld* World = Scene.GetTestWorld();
    FAngelIOStartupProbe Probe(bReuseBytecode ? 1 : 0);
    TArray<TUniquePtr<FAngelIOProgram>> Missions;
    for (int32 I = 0; I < Count; ++I)
    {
        auto Mission = MakeUnique<FAngelIOProgram>();
        FAngelIOValue Floor;
        Floor.Integer = I + 1;
        TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
        if (!Mission->CompileFile(TEXT("Elevator.as"), World) || !Mission->Start({{TEXT("StartingFloor"), Floor}}))
        {
            Test.AddError(FString::Printf(TEXT("Could not start shared mission %d"), I));
            return false;
        }
        Missions.Add(MoveTemp(Mission));
    }
    if (!Test.TestEqual(TEXT("All 1000 modules use one API registration"), Probe.Registrations, 1) ||
        !Test.TestEqual(TEXT("Source builds before invalid edit"), Probe.Builds, bReuseBytecode ? 1 : Count) ||
        !Test.TestEqual(TEXT("Bytecode loads before invalid edit"), Probe.Loads, bReuseBytecode ? Count - 1 : 0)) return false;
    {
        FAngelIOProgram Invalid;
        TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
        if (!Test.TestFalse(TEXT("Changed source under the same name fails to build"), Invalid.Compile(TEXT("Elevator.as"), TEXT("void Run() {\nUnknown();\n}"), World)) ||
            !Test.TestTrue(TEXT("The failing module owns its diagnostic"), Invalid.Diagnostics().ContainsByPredicate(
                [](const auto& D) { return D.Severity == TEXT("error") && D.Line == 2; })) ||
            !Test.TestTrue(TEXT("A valid sibling retains clean diagnostics"), Missions.Last()->Diagnostics().IsEmpty())) return false;
    }
    const auto Lifetime = FAngelIOStartupProbe::WorldEngine(World);
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
            Test.AddError(FString::Printf(TEXT("Mission %d has incorrect globals, wait or source location"), I));
            return false;
        }
        Instances.Add(Site.Instance);
    }
    if (!Test.TestEqual(TEXT("Distinct mission IDs"), Instances.Num(), Count)) return false;
    auto Survivor = MoveTemp(Missions.Last());
    Missions.Reset();
    if (!Test.TestEqual(TEXT("Only the last module remains"), FAngelIOStartupProbe::WorldModuleCount(World), 1) ||
        !Test.TestTrue(TEXT("The survivor can resume after the first module was discarded"), Survivor->Signal(TEXT("RequestFloor"), 4)) ||
        !Test.TestEqual(TEXT("The survivor advances to its own motion wait"), Survivor->InspectMission().Line, 25)) return false;
    if (!Scene.DestroyTestWorld(true)) return false;
    return Test.TestFalse(TEXT("World teardown releases the registered engine"), Lifetime.IsValid()) &&
        Test.TestTrue(TEXT("Retained native program stops at world teardown"), Survivor->State() == EAngelIOState::Failed &&
            !Survivor->Signal(TEXT("MotionFinished"), 4));
}

bool FAngelIOSharedIsolation::RunTest(const FString&) { return CheckIsolation(*this, false); }
bool FAngelIOBytecodeIsolation::RunTest(const FString&) { return CheckIsolation(*this, true); }
#endif
